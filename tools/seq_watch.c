/* Probe: subscribe to "Akai Pro Force Private" and print every event. */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    snd_seq_t *s;
    if (snd_seq_open(&s, "default", SND_SEQ_OPEN_INPUT, 0) < 0) { perror("open"); return 1; }
    snd_seq_set_client_name(s, "force_shadow_watch");
    int me = snd_seq_create_simple_port(s, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (me < 0) { fprintf(stderr, "create_port failed: %d\n", me); return 1; }
    snd_seq_client_info_t *ci; snd_seq_port_info_t *pi;
    snd_seq_client_info_alloca(&ci); snd_seq_port_info_alloca(&pi);
    snd_seq_client_info_set_client(ci, -1);
    int found = 0;
    while (snd_seq_query_next_client(s, ci) >= 0) {
        int c = snd_seq_client_info_get_client(ci);
        snd_seq_port_info_set_client(pi, c); snd_seq_port_info_set_port(pi, -1);
        while (snd_seq_query_next_port(s, pi) >= 0) {
            if (strstr(snd_seq_port_info_get_name(pi), "Force Private")) {
                int p = snd_seq_port_info_get_port(pi);
                int r = snd_seq_connect_from(s, me, c, p);
                fprintf(stderr, "subscribe %d:%d -> %d\n", c, p, r);
                found = 1;
            }
        }
    }
    if (!found) { fprintf(stderr, "Force Private not found\n"); return 1; }
    fprintf(stderr, "READY\n");
    snd_seq_event_t *ev;
    while (snd_seq_event_input(s, &ev) >= 0) {
        if (ev->type == SND_SEQ_EVENT_SYSEX) continue;
        if (ev->type == SND_SEQ_EVENT_NOTEON || ev->type == SND_SEQ_EVENT_NOTEOFF)
            printf("type=%d note ch=%d note=%d vel=%d\n", ev->type, ev->data.note.channel, ev->data.note.note, ev->data.note.velocity);
        else if (ev->type == SND_SEQ_EVENT_CONTROLLER)
            printf("type=%d cc ch=%d param=%d val=%d\n", ev->type, ev->data.control.channel, ev->data.control.param, ev->data.control.value);
        else
            printf("type=%d other\n", ev->type);
        fflush(stdout);
    }
    return 0;
}
