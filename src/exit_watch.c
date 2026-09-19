/* force_shadow_exitwatch: leave shadow mode when a mode button is pressed.
 *
 * Runs as its own process (started by run_ForceShadow.sh), NOT inside MPC,
 * so libasound never loads into the Force app. Subscribes to "Akai Pro
 * Force Private" and, on a press of MENU/LOAD/SAVE/MATRIX/CLIP/MIXER/
 * NAVIGATE/KNOBS, removes the shadow toggle files (same exit as pressing
 * the KNOBS+SCENE-N combo again). MPC still receives the press normally.
 *
 * Note numbers captured live from the device (2026-09-19).
 * KNOBS is also the modifier of the KNOBS+SCENE-N combo, so it exits on
 * RELEASE and only if no other note arrived while it was held; otherwise
 * the combo's own SCENE-N press would immediately reopen the page. */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NOTE_KNOBS 1
static const int exit_notes[] = { 2 /*MENU*/, 35 /*LOAD*/, 36 /*SAVE*/, 3 /*MATRIX*/,
                                  9 /*CLIP*/, 11 /*MIXER*/, 0 /*NAVIGATE*/ };

static void leave_shadow(void) {
    unlink("/tmp/force_shadow_on");
    unlink("/tmp/force_shadow_page");
}

static int connect_private(snd_seq_t *s, int me) {
    snd_seq_client_info_t *ci; snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci); snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(s, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c);
        snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(s, pi) >= 0)
            if (strstr(snd_seq_port_info_get_name(pi), "Force Private") &&
                snd_seq_connect_from(s, me, c, snd_seq_port_info_get_port(pi)) == 0)
                return 0;
    }
    return -1;
}

int main(void) {
    for (;;) {  /* retry forever: the port may not exist yet at boot */
        snd_seq_t *s;
        if (snd_seq_open(&s, "default", SND_SEQ_OPEN_INPUT, 0) < 0) { sleep(5); continue; }
        snd_seq_set_client_name(s, "force_shadow_exitwatch");
        int me = snd_seq_create_simple_port(s, "in",
                     SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                     SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (me < 0 || connect_private(s, me) < 0) { snd_seq_close(s); sleep(5); continue; }

        int knobs_down = 0, knobs_combo = 0;
        snd_seq_event_t *ev;
        while (snd_seq_event_input(s, &ev) >= 0) {
            if (ev->type != SND_SEQ_EVENT_NOTEON && ev->type != SND_SEQ_EVENT_NOTEOFF) continue;
            int n = ev->data.note.note;
            int press = (ev->type == SND_SEQ_EVENT_NOTEON && ev->data.note.velocity > 0);
            if (n == NOTE_KNOBS) {
                if (press) { knobs_down = 1; knobs_combo = 0; }
                else { if (knobs_down && !knobs_combo) leave_shadow(); knobs_down = 0; }
                continue;
            }
            if (!press) continue;
            if (knobs_down) knobs_combo = 1;
            for (unsigned i = 0; i < sizeof exit_notes / sizeof *exit_notes; i++)
                if (n == exit_notes[i]) { leave_shadow(); break; }
        }
        snd_seq_close(s);
        sleep(2);
    }
}
