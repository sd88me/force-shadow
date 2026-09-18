/* seq_probe.c -- read-only ALSA sequencer diagnostic.
 *
 * Companion to atomic_probe.c/getfb.c's DRM-side discovery methodology
 * (see DESIGN.md), same project, different subsystem: finds a named
 * ALSA seq client (substring match against argv[1], e.g. "Akai Pro
 * Force Private") and dumps every event it sends, so a human can
 * correlate physical button presses on the Force's own control surface
 * with their actual note/CC identities -- needed because MidiLoop's own
 * config only intercepts a small subset of buttons (modifier combos,
 * a couple of named HW-* triggers); most buttons (LOAD/SAVE/MATRIX/
 * CLIP/MIXER/NAVIGATE/LAUNCH/MENU when pressed unmodified) go straight
 * to MPC's own firmware with no config-level hook at all.
 *
 * Read-only and low-risk: subscribes for READ only (SNDRV_SEQ_PORT_CAP_
 * WRITE on our own port just means "others can write events INTO us",
 * a normal MIDI input port -- this process never sends anything out).
 * The one ALSA seq client/port this creates is owned by this process's
 * fd and is cleaned up automatically by the kernel when it exits or the
 * fd closes, the same way DRM resources tied to an fd are torn down on
 * close -- no persistent state left behind either way.
 *
 * Uses the real kernel UAPI header (<sound/asequencer.h>, from
 * linux-libc-dev) rather than hand-typing structs from memory the way
 * force_shadow.c's DRM structs had to be (no libdrm/kernel headers were
 * available in that case) -- this is compile-time only, no runtime
 * library dependency, so it doesn't touch this project's own
 * libc/libpthread/libdl-only philosophy for the actual shared library.
 *
 * Usage: seq_probe "<substring of target client name>"
 *        e.g. seq_probe "Akai Pro Force Private"
 * Ctrl-C to stop.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sound/asequencer.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <substring-of-client-name>\n", argv[0]);
        return 1;
    }
    const char *target_name = argv[1];

    int fd = open("/dev/snd/seq", O_RDWR);
    if (fd < 0) { perror("open /dev/snd/seq"); return 1; }

    /* Mystery ioctl seen via strace on arecordmidi (a known-working
     * client on this device), called immediately after PVERSION and
     * before anything else -- not even named in this device's own
     * strace's ioctl table, so likely newer than any header we have
     * access to (probably a MIDI2/UMP protocol-negotiation call added
     * post-6.5). Replicating it verbatim, with a 0 ("legacy"?) argument,
     * to see whether it's a precondition for CREATE_PORT succeeding. */
    int pversion = 0;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_PVERSION, &pversion) < 0) {
        perror("PVERSION (continuing anyway)");
    }
    int mystery_arg = 0;
    long unk = ioctl(fd, _IOW('S', 0x04, int), &mystery_arg);
    fprintf(stderr, "mystery ioctl 0x04 result: %ld (errno %d: %s)\n",
             unk, errno, strerror(errno));

    int my_client = 0;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &my_client) < 0) {
        perror("CLIENT_ID"); return 1;
    }
    fprintf(stderr, "our client id: %d\n", my_client);

    struct snd_seq_running_info rinfo;
    memset(&rinfo, 0, sizeof(rinfo));
    if (ioctl(fd, SNDRV_SEQ_IOCTL_RUNNING_MODE, &rinfo) < 0) {
        perror("RUNNING_MODE (continuing anyway)");
    }

    fprintf(stderr, "--- enumerating clients/ports ---\n");
    struct snd_seq_client_info cinfo;
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.client = -1;
    int found_client = -1, found_port = -1;
    while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &cinfo) >= 0) {
        fprintf(stderr, "client %d: '%s'\n", cinfo.client, cinfo.name);
        struct snd_seq_port_info pinfo;
        memset(&pinfo, 0, sizeof(pinfo));
        pinfo.addr.client = (unsigned char)cinfo.client;
        pinfo.addr.port = 255; /* -1 as unsigned char: "start from beginning" */
        while (ioctl(fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT, &pinfo) >= 0) {
            fprintf(stderr, "  port %d:%d '%s' cap=0x%x type=0x%x\n",
                     pinfo.addr.client, pinfo.addr.port, pinfo.name,
                     pinfo.capability, pinfo.type);
            if (found_client < 0 &&
                (strstr(cinfo.name, target_name) || strstr(pinfo.name, target_name))) {
                found_client = pinfo.addr.client;
                found_port = pinfo.addr.port;
            }
        }
    }
    fprintf(stderr, "--- end enumeration ---\n");

    if (found_client < 0) {
        fprintf(stderr, "no client matching '%s' found\n", target_name);
        return 1;
    }
    fprintf(stderr, "target: client %d port %d\n", found_client, found_port);

    struct snd_seq_client_info myclient;
    memset(&myclient, 0, sizeof(myclient));
    myclient.client = my_client;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_GET_CLIENT_INFO, &myclient) < 0) {
        perror("GET_CLIENT_INFO");
    }
    fprintf(stderr, "our client type=%d midi_version=%u (pre-set)\n",
             myclient.type, myclient.midi_version);
    strncpy(myclient.name, "seq_probe", sizeof(myclient.name) - 1);
    if (ioctl(fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &myclient) < 0) {
        perror("SET_CLIENT_INFO (continuing anyway)");
    }

    /* arecordmidi (a known-working client on this device, confirmed via
     * strace) creates a queue before creating its port. Architecturally
     * unrelated concepts on stock ALSA, but replicating verbatim since
     * reasoning about this custom kernel hasn't converged -- empirical
     * beats theoretical here. */
    struct snd_seq_queue_info qinfo;
    memset(&qinfo, 0, sizeof(qinfo));
    qinfo.queue = -1; /* request auto-assigned queue id */
    qinfo.owner = my_client;
    qinfo.locked = 1;
    strncpy(qinfo.name, "seq_probe", sizeof(qinfo.name) - 1);
    if (ioctl(fd, SNDRV_SEQ_IOCTL_CREATE_QUEUE, &qinfo) < 0) {
        perror("CREATE_QUEUE (continuing anyway)");
    } else {
        fprintf(stderr, "created queue %d\n", qinfo.queue);
        struct snd_seq_queue_tempo qtempo;
        memset(&qtempo, 0, sizeof(qtempo));
        qtempo.queue = qinfo.queue;
        qtempo.tempo = 500000;
        qtempo.ppq = 480;
        if (ioctl(fd, SNDRV_SEQ_IOCTL_SET_QUEUE_TEMPO, &qtempo) < 0) {
            perror("SET_QUEUE_TEMPO (continuing anyway)");
        }
    }

    /* Matched byte-for-byte against a known-working client (arecordmidi)
     * via a dedicated LD_PRELOAD dump (tools/seq_dump_ioctl.c) after
     * plain APPLICATION-type/zeroed-channels/no-queue-link got EPERM
     * here but not there. Three real differences found: `type` needs
     * MIDI_GENERIC alongside APPLICATION, `midi_channels` needs to be
     * nonzero (16, matching a real MIDI port), and `flags`/`time_queue`
     * need to reference the queue just created above -- a zeroed/
     * unlinked port apparently isn't accepted as a valid MIDI port by
     * this kernel's CREATE_PORT validation, where stock ALSA is more
     * permissive. */
    struct snd_seq_port_info myport;
    memset(&myport, 0, sizeof(myport));
    strncpy(myport.name, "seq_probe", sizeof(myport.name) - 1);
    myport.capability = SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
    myport.type = SNDRV_SEQ_PORT_TYPE_APPLICATION | SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC;
    myport.midi_channels = 16;
    myport.flags = SNDRV_SEQ_PORT_FLG_GIVEN_PORT | SNDRV_SEQ_PORT_FLG_TIMESTAMP;
    myport.time_queue = (unsigned char)qinfo.queue;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &myport) < 0) {
        perror("CREATE_PORT"); return 1;
    }
    fprintf(stderr, "our port: %d:%d\n", myport.addr.client, myport.addr.port);

    struct snd_seq_port_subscribe sub;
    memset(&sub, 0, sizeof(sub));
    sub.sender.client = (unsigned char)found_client;
    sub.sender.port = (unsigned char)found_port;
    sub.dest.client = (unsigned char)myport.addr.client;
    sub.dest.port = (unsigned char)myport.addr.port;
    if (ioctl(fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0) {
        perror("SUBSCRIBE_PORT"); return 1;
    }
    fprintf(stderr, "subscribed. reading events (Ctrl-C to stop)...\n");

    signal(SIGINT, on_sigint);
    struct snd_seq_event ev;
    while (!g_stop) {
        ssize_t r = read(fd, &ev, sizeof(ev));
        if (r != (ssize_t)sizeof(ev)) continue;
        if (ev.type == SNDRV_SEQ_EVENT_NOTEON || ev.type == SNDRV_SEQ_EVENT_NOTEOFF ||
            ev.type == SNDRV_SEQ_EVENT_NOTE || ev.type == SNDRV_SEQ_EVENT_KEYPRESS) {
            printf("NOTE type=%u chan=%u note=%u vel=%u\n",
                    ev.type, ev.data.note.channel, ev.data.note.note, ev.data.note.velocity);
        } else if (ev.type == SNDRV_SEQ_EVENT_CONTROLLER) {
            printf("CC   chan=%u param=%u value=%d\n",
                    ev.data.control.channel, ev.data.control.param, ev.data.control.value);
        } else {
            printf("other type=%u\n", ev.type);
        }
        fflush(stdout);
    }

    return 0;
}
