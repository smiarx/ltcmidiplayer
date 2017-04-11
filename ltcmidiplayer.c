#define NAME "ltcmidiplayer"
//#define VERBOSE
#define RELEASE_WHEN_STOP // send release control when stop
//#define RELEASE_WHEN_DISCONTINUITY //send release control when discontinuity
#define SEND_PITCH_CONTROL
#define TRANSPOSE_CC 112

#define LTC_QUEUE_LEN (42)
#define SKEW_BASE (0x10000)
#define CHANGE_SPEED_THRESH (SKEW_BASE >> 10)
#define FRAMES_BUFFERING (90)


#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <signal.h>

#include <pthread.h>
#include <jack/jack.h>
#include <ltc.h>
#include <alsa/asoundlib.h>

#include "ltcframeutil.h"


#ifdef VERBOSE
static int doverbose = 1;
#else
static int doverbose = 0;
#endif


static jack_client_t *jclient;
static jack_port_t *jinput;
static jack_nframes_t jlatency = 0;
static unsigned char *audioin = NULL;

static volatile ltc_off_t posinfo = 0;
static LTCDecoder *ltcdecoder = NULL;
static int fps;
static int fpsnum = 30;
static int fpsden = 1;
static int cycles_between_frames;//number of cycles between frames, until we stop
static int samples_per_frame_skew;
#ifdef SEND_PITCH_CONTROL
static int skewpitchbias;
#endif


//midi
struct event {
    struct event* next;
    struct event* prev; //double linked list

    unsigned char type; //SND_SEQ_EVENT_xxx
    unsigned char port; // port index
    unsigned int  tick;
    union{
        unsigned char d[3];
        int tempo;
        unsigned int length;//length sysex data
    } data;
    unsigned char sysex[0];
};

struct track{
    struct event *first;
    struct event *last;
    int end_tick;
    struct event *current;
};

static snd_seq_t *seq;
static int mclient;
static int mqueue;
static int ntracks;
static struct track* tracks;
static int issmpte;
static int ticks_per_frame;


static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  thread_cond  = PTHREAD_COND_INITIALIZER;

static volatile enum {Run,Exit} state = Run;




static inline void verbose(const char* msg,...){
    if(!doverbose) return;
    va_list ap;
    va_start(ap,msg);
    vfprintf(stdout, msg, ap);
    va_end(ap);
    fputc('\n', stdout);
}


static inline void error(const char* msg,...){
    va_list ap;
    va_start(ap,msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static inline void fatal(const char* msg,...){
    va_list ap;
    va_start(ap,msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}



void cleanup(void){
    if(jclient){
        jack_client_close(jclient);
        jclient=NULL;
    }
    if(audioin){
        free(audioin);
        audioin=NULL;
    }
    ltc_decoder_free(ltcdecoder);
    snd_seq_close(seq);
    verbose("byebye");
}



void catchsig(int sig){
    signal(SIGHUP, catchsig);
    verbose("shutdown");
    state=Exit;
    pthread_cond_signal(&thread_cond);
}









static inline int frame2tick(LTCFrameExt* frame){
    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &(frame->ltc), 0);
    return ((3600*stime.hours + 60*stime.mins + stime.secs)* fpsnum/fpsden + stime.frame)*ticks_per_frame;
}






static void loop(void){
    LTCFrameExt frame, prevframe;
    snd_seq_event_t ev, timerev, ctrlev;
    snd_seq_queue_status_t* status;
    int noframecycles=0, stopped=1;
    int skew;

#ifdef SEND_PITCH_CONTROL
    int pitchbend;
    int prevskew=0;
    snd_seq_event_t pitchev;

    //set pitch bend event
    snd_seq_ev_clear(&pitchev);
    snd_seq_ev_set_fixed(&pitchev);
    pitchev.type = SND_SEQ_EVENT_PITCHBEND;
    pitchev.data.control.channel = 0;
    pitchev.source.port = 0;
    pitchev.queue = SND_SEQ_QUEUE_DIRECT;
    snd_seq_ev_set_subs(&pitchev);
#endif


    snd_seq_queue_status_alloca(&status);

    // common settings for all events
    snd_seq_ev_clear(&ev);
    ev.queue = mqueue;
    ev.source.port = 0;
    ev.flags = SND_SEQ_TIME_STAMP_TICK;
    snd_seq_ev_set_subs(&ev);

    //position event
    snd_seq_ev_clear(&timerev);
    snd_seq_ev_set_dest(&timerev, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_TIMER);
    timerev.data.queue.queue = mqueue;
    timerev.queue = SND_SEQ_QUEUE_DIRECT;

    //cc event
    snd_seq_ev_clear(&ctrlev);
    snd_seq_ev_set_fixed(&ctrlev);
    ctrlev.source.port = 0;
    ctrlev.queue = SND_SEQ_QUEUE_DIRECT;
    snd_seq_ev_set_subs(&ctrlev);
    ctrlev.type=SND_SEQ_EVENT_CONTROLLER;


    pthread_mutex_lock(&thread_mutex);

    //start queue & let it hang on
    snd_seq_start_queue(seq, mqueue, NULL);
    snd_seq_stop_queue(seq, mqueue, NULL);
    snd_seq_drain_output(seq);

    while(state==Run){

        while(ltc_decoder_read(ltcdecoder,&frame)){
            int tick, max_tick;
            noframecycles=0;

            //get tick
            tick = frame2tick(&frame);
            max_tick=tick+FRAMES_BUFFERING*ticks_per_frame;//output until this tick


            //DISCONTINUITY
            if(detect_discontinuity(&frame, &prevframe,fps,0,0)){
                struct track *track_ptr;
                int i;

                verbose("DISCONTINUITY");
                snd_seq_drop_output(seq);//drop all events

#ifdef RELEASE_WHEN_DISCONTINUITY
                /* send CC123 -> release all voices */
                ctrlev.data.control.channel = 0;
                ctrlev.data.control.param = 123;
                ctrlev.data.control.value = 127;
                snd_seq_event_output(seq, &ctrlev);
#endif

                //replace tracks cursors
                track_ptr = tracks;
                for(i=0; i<ntracks;++i){
                    struct event* midiev= track_ptr->current ? track_ptr->current : track_ptr->last;//if end have been reached
                    if(midiev->tick < tick){ //forward
                        while(midiev && midiev->tick < tick) midiev = midiev->next;
                        track_ptr->current = midiev;

                    } else if(midiev->tick > tick){ //rewind
                        while(midiev && midiev->tick > tick)
                            midiev = midiev->prev;
                        if(midiev) 
                            track_ptr->current = midiev->next;
                        else
                            track_ptr->current = track_ptr->first; 
                    }
                    track_ptr++;
                }
            }


            //set queue position
            timerev.type = SND_SEQ_EVENT_SETPOS_TICK;
            timerev.data.queue.param.time.tick = tick;
            snd_seq_event_output(seq, &timerev);


            //set skew (speed)
            timerev.type=SND_SEQ_EVENT_QUEUE_SKEW;
            skew = samples_per_frame_skew/(frame.off_end-frame.off_start);
            timerev.data.queue.param.skew.value = skew;
            timerev.data.queue.param.skew.base = SKEW_BASE;
            snd_seq_event_output(seq, &timerev);
#ifdef SEND_PITCH_CONTROL
            if(abs(skew-prevskew) > CHANGE_SPEED_THRESH){//speed has changed
                pitchbend = log2(skew)*8192*6 - skewpitchbias;

                //send pitch bend
                pitchev.data.control.value = pitchbend%8192;
                snd_seq_event_output(seq, &pitchev);

                //send cc transposition
                ctrlev.data.control.channel = 0;
                ctrlev.data.control.param = TRANSPOSE_CC;
                ctrlev.data.control.value = pitchbend/8192+64;
                snd_seq_event_output(seq, &ctrlev);
                //verbose("%d - %d - %f", abs(skew-prevskew), pitchbend, (float) skew/SKEW_BASE);

                prevskew=skew;
            }

#endif


            //we restart if we have been stopped
            if(stopped){
                stopped=0;
                verbose("PLAY");

                
                //conitnue queue
                snd_seq_continue_queue(seq, mqueue, NULL);
            }


            // common settings for all events
            snd_seq_ev_clear(&ev);
            ev.queue = mqueue;
            ev.source.port = 0;
            ev.flags = SND_SEQ_TIME_STAMP_TICK;
            snd_seq_ev_set_subs(&ev);

            for(;;){
                struct event *event = NULL;
                struct track *evtrack = NULL;
                int i, min_tick = max_tick+1;

                for(i=0; i<ntracks; ++i){
                    struct track* track = &tracks[i];
                    struct event* e2 = track->current;
                    if(e2 && e2->tick < min_tick){
                        event=e2;
                        evtrack = track;
                    }
                }

                if(!event)
                    break;//wait until buffering next events
                evtrack->current = event->next;

                //outputing event
                ev.type = event->type;
                ev.time.tick = event->tick;
                switch (ev.type) {
                    case SND_SEQ_EVENT_NOTEON:
                    case SND_SEQ_EVENT_NOTEOFF:
                    case SND_SEQ_EVENT_KEYPRESS:
                        snd_seq_ev_set_fixed(&ev);
                        ev.data.note.channel = event->data.d[0];
                        ev.data.note.note = event->data.d[1];
                        ev.data.note.velocity = event->data.d[2];
                        //verbose("\tnote %d - %d", event->data.d[1], event->data.d[2]);
                        break;
                    case SND_SEQ_EVENT_CONTROLLER:
                        snd_seq_ev_set_fixed(&ev);
                        ev.data.control.channel = event->data.d[0];
                        ev.data.control.param = event->data.d[1];
                        ev.data.control.value = event->data.d[2];
                        break;
                    case SND_SEQ_EVENT_PGMCHANGE:
                    case SND_SEQ_EVENT_CHANPRESS:
                        snd_seq_ev_set_fixed(&ev);
                        ev.data.control.channel = event->data.d[0];
                        ev.data.control.value = event->data.d[1];
                        break;
                    case SND_SEQ_EVENT_PITCHBEND:
                        snd_seq_ev_set_fixed(&ev);
                        ev.data.control.channel = event->data.d[0];
                        ev.data.control.value =
                            ((event->data.d[1]) |
                             ((event->data.d[2]) << 7)) - 0x2000;
                        break;
                    case SND_SEQ_EVENT_SYSEX:
                        continue; //#yolo
                        //snd_seq_ev_set_variable(&ev, event->data.length,
                        //            event->sysex);
                        //handle_big_sysex(&ev);
                        break;
                    case SND_SEQ_EVENT_TEMPO:
                        snd_seq_ev_set_fixed(&ev);
                        ev.dest.client = SND_SEQ_CLIENT_SYSTEM;
                        ev.dest.port = SND_SEQ_PORT_SYSTEM_TIMER;
                        ev.data.queue.queue = mqueue;
                        ev.data.queue.param.value = event->data.tempo;
                        break;
                    default:
                        fatal("Invalid event type %d!", ev.type);
                }

                snd_seq_event_output(seq, &ev);
                
            }


            //empty buffer
            snd_seq_drain_output(seq);
            snd_seq_get_queue_status(seq,mqueue,status);
            //verbose("current tick %d --- %d", tick, snd_seq_queue_status_get_tick_time(status));

        }


        //if too many iterations with no frame -> stop
        if(!stopped && ++noframecycles>cycles_between_frames){
            stopped=1;
            verbose("STOP");
            snd_seq_stop_queue(seq, mqueue, NULL);

#ifdef RELEASE_WHEN_STOP
            /* send CC123 -> release all voices */
            ctrlev.data.control.channel = 0;
            ctrlev.data.control.param = 123;
            ctrlev.data.control.value = 127;
            snd_seq_event_output(seq, &ctrlev);
#endif


            snd_seq_drain_output(seq);
        }


        pthread_cond_wait(&thread_cond, &thread_mutex);
    }
    pthread_mutex_unlock(&thread_mutex);
}






/* FILE READING */
static inline int read_byte(FILE* file)
{
	//++file_offset;
	return getc(file);
}

/* reads a little-endian 32-bit integer */
static int read_32_le(FILE* file)
{
	int value;
	value = read_byte(file);
	value |= read_byte(file) << 8;
	value |= read_byte(file) << 16;
	value |= read_byte(file) << 24;
	return !feof(file) ? value : -1;
}


/* reads a fixed-size big-endian number */
static int read_int(int bytes, FILE* file)
{
	int c, value = 0;

	do {
		c = read_byte(file);
		if (c == EOF)
			return -1;
		value = (value << 8) | c;
	} while (--bytes);
	return value;
}

/* reads a variable-length number */
static int read_var(FILE* file, int *off)
{
	int value, c;
    int l=1;

	c = read_byte(file);
	value = c & 0x7f;
	if (c & 0x80) {
		c = read_byte(file);
		value = (value << 7) | (c & 0x7f);
        l++;
		if (c & 0x80) {
			c = read_byte(file);
			value = (value << 7) | (c & 0x7f);
            l++;
			if (c & 0x80) {
				c = read_byte(file);
				value = (value << 7) | c;
                l++;
				if (c & 0x80)
					return -1;
			}
		}
	}
    if(off) off+=l;
	return !feof(file) ? value : -1;
}


static inline void skip(int bytes, FILE* file){
    fseek(file,bytes,SEEK_CUR);
}



#define SMF_ID(c1,c2,c3,c4) ((c1) | ((c2) << 8) | ((c3) << 16) | ((c4) << 24))


/* MIDI */

static struct event* new_event(struct track *track, int sysex_length){
    struct event* event;
    event = malloc(sizeof(struct event) + sysex_length);

    event->next = NULL;
    event->prev = NULL;
    if(track->current){
        track->current->next = event;
        event->prev = track->current;
    } else
        track->first = event;
    track->current = event;

    return event;
}

static int read_track(FILE* file, int tracklen, struct track* track){
    int l=0;
    int tick=0;
    unsigned char last_cmd = 0;

    track->current = NULL;
    while(l < tracklen){
        unsigned char cmd;
        struct event *event;
        int delta_ticks, c;

        delta_ticks = read_var(file, &l);
        if(delta_ticks < 0) break;
        tick += delta_ticks;

        c = read_byte(file);
        l++;
        if(c<0) break;

        if(c & 0x80){
            cmd = c;
            if(cmd < 0xf0)
                last_cmd = cmd;
        } else {
            ungetc(c, file);
            l--;
            cmd = last_cmd;
            if(!cmd)
                fatal("?");
        }

        switch (cmd >> 4){
            // SMF event -> alsa event mapping
            static const unsigned char cmd_type[] = {
                [0x8] = SND_SEQ_EVENT_NOTEOFF,
                [0x9] = SND_SEQ_EVENT_NOTEON,
                [0xa] = SND_SEQ_EVENT_KEYPRESS,
                [0xb] = SND_SEQ_EVENT_CONTROLLER,
                [0xc] = SND_SEQ_EVENT_PGMCHANGE,
                [0xd] = SND_SEQ_EVENT_CHANPRESS,
                [0xe] = SND_SEQ_EVENT_PITCHBEND,
            };
            case 0x8:
            case 0x9:
            case 0xa:
            case 0xb:
            case 0xe:
                event = new_event(track, 0);
                event->type = cmd_type[cmd>>4];
                event->tick = tick;
                event->data.d[0] = cmd & 0x0f;
                event->data.d[1] = read_byte(file) & 0x7f;
                event->data.d[2] = read_byte(file) & 0x7f;
                l+=2;
                break;
            case 0xc:
            case 0xd:
                event = new_event(track, 0);
                event->type = cmd_type[cmd>>4];
                event->tick = tick;
                event->data.d[0] = cmd & 0x0f;
                event->data.d[1] = read_byte(file) & 0x7f;
                l++;
                break;

            /* SYSEX / META */
            case 0xf:
                switch (cmd) {
                    int len;
                    case 0xf0: /* sysex */
                    case 0xf7: /* continued sysex, or escaped commands */
                        len = read_var(file, &l);
                        if (len < 0)
                            goto _error;
                        if (cmd == 0xf0)
                            ++len;
                        event = new_event(track, len);
                        event->type = SND_SEQ_EVENT_SYSEX;
                        //event->port = port;
                        event->tick = tick;
                        event->data.length = len;
                        if (cmd == 0xf0) {
                            event->sysex[0] = 0xf0;
                            c = 1;
                        } else {
                            c = 0;
                        }
                        for (; c < len; ++c){
                            event->sysex[c] = read_byte(file);
                            l++;
                        }
                        break;

                    case 0xff: /* meta event */
                        c = read_byte(file);
                        len = read_var(file, &l);
                        l++;
                        if (len < 0)
                            goto _error;

                        switch (c) {
                            case 0x21: /* port number */
                                if (len < 1)
                                    goto _error;
                                //port = read_byte() % port_count;
                                read_byte(file);
                                skip(len - 1, file);
                                l+=len;
                                break;

                            case 0x2f: /* end of track */
                                track->end_tick = tick;
                                skip(tracklen - l, file);
                                l=tracklen;
                                return 1;

                            case 0x51: /* tempo */
                                if (len < 3)
                                    goto _error;
                                if (issmpte) {
                                    /* SMPTE timing doesn't change */
                                    skip(len, file);
                                    l+=len;
                                } else {
                                    event = new_event(track, 0);
                                    event->type = SND_SEQ_EVENT_TEMPO;
                                    //event->port = port;
                                    event->tick = tick;
                                    event->data.tempo = read_byte(file) << 16;
                                    event->data.tempo |= read_byte(file) << 8;
                                    event->data.tempo |= read_byte(file);
                                    skip(len - 3, file);
                                    l+=len;
                                }
                                break;

                            default: /* ignore all other meta events */
                                skip(len, file);
                                l+=len;
                                break;
                            }
                        break;
                    default: /* invalid Fx command */
                        goto _error;
                    }
                break;
            default: /* cannot happen */
                goto _error;
        }

    }
_error:
    error("invalid midi");
    return 0;
}



static int read_smf(char *filename){
    FILE* file;

    if(!strcmp(filename, "-"))
        file = stdin;
    else
        file = fopen(filename, "rb");
    if(!file)
        fatal("cannot open %s", filename);

    //file ID
    switch(read_32_le(file)){
        case SMF_ID('M', 'T', 'h', 'd'):
            //ok
            break;
        case SMF_ID('R','I','F','F'):
            error("unable to read RIFF midi files");
            return 0;
            break;
        default:
            error("unknown midi format");
            return 0;
    } 

    /*READ SMF*/
    int header_len, type, timediv;
    snd_seq_queue_tempo_t *queue_tempo;
    int tempo, ppq;

    if((header_len = read_int(4,file)) < 6){
        error("invalid file format");
        return 0;
    }

    type = read_int(2, file);
    if(type !=0 && type != 1){
        error("type %d format no supported", type);
        return 0;
    }

    ntracks = read_int(2, file);
    if(ntracks < 1 || ntracks > 1000){
        error("invalid number of tracks: %d", ntracks);
        return 0;
    }
    verbose("%d midi tracks", ntracks);
    tracks = calloc(ntracks, sizeof(struct track));

    timediv = read_int(2, file);
    
    issmpte = !!(timediv & 0x8000);
    if(issmpte){ //msbit SMPTE timing;
        int mfps = 0x80 - ((timediv >> 8) & 0x7f);//upper byte, negative fps
        timediv &= 0xff;

        switch (mfps){
            case 24:
                tempo = 500000;
                ppq = 12*timediv;
                break;
            case 25:
                tempo = 400000;
                ppq = 10*timediv;
                break;
            case 29://30 drop frame
                tempo = 100000000;
                ppq = 2997*timediv;
                break;
            case 30:
                tempo = 500000;
                ppq = 15 * timediv;
                break;
            default:
                error("invalid SMPTE fps: %d", mfps);
                return 0;
        }
    } else {
        tempo = 500000;
        ppq = timediv;
    }
    snd_seq_queue_tempo_alloca(&queue_tempo);
    snd_seq_queue_tempo_set_tempo(queue_tempo, tempo);
    snd_seq_queue_tempo_set_ppq(queue_tempo, ppq);

    snd_seq_queue_tempo_set_skew(queue_tempo, SKEW_BASE);
    snd_seq_queue_tempo_set_skew_base(queue_tempo, SKEW_BASE);

    if(snd_seq_set_queue_tempo(seq, mqueue, queue_tempo) < 0){
        error("cannot set queue tempo");
        return 0;
    }

    ticks_per_frame = (long) ppq*1000000*fpsden/(tempo*fpsnum);
    verbose("%d", ticks_per_frame);



    //read tracks
    int i;
    struct track *track_ptr = tracks;
    for(i = 0; i<ntracks; ++i){
        int len;
        for(;;){
            int id = read_32_le(file);
            len = read_int(4, file);
            if(feof(file)){
                error("unexpected EOF");
                return 0;
            }
            if(len < 0 || len >= 0x10000000){
                error("invalid chunck length: %d", len);
                return 0;
            }
            if(id == SMF_ID('M','T','r','k'))
                break;
            skip(len,file);
        }
        if(!read_track(file, len, track_ptr))
            return 0;
        track_ptr->last = track_ptr->current;
        track_ptr->current = track_ptr->first;
        track_ptr++;
    }

    return 1;
}










/* JACK CALLBACKS */
int process(jack_nframes_t nframes, void* arg){
    jack_nframes_t i;
    jack_default_audio_sample_t *in;
    unsigned char *audioin_ptr = audioin;

    in = jack_port_get_buffer(jinput, nframes);

    //convert audio from 32bfloat to 8bit
    for(i=0; i<nframes; ++i){
        const int snd = (int)rint((127.0*(*in++))+128.0);
        *audioin_ptr++ = (unsigned char) (snd&0xff);
    }
    //write to decoder
    ltc_decoder_write(ltcdecoder, audioin, nframes, posinfo);
    posinfo += nframes;

    //signal the playing loop
    if(pthread_mutex_trylock (&thread_mutex) == 0){
        pthread_cond_signal (&thread_cond);
        pthread_mutex_unlock (&thread_mutex);
    }
    return 0;
}

void jack_shutdown(void *arg){
    cleanup();
    fatal("JACK shutdown");
}


int jack_bufsize(jack_nframes_t nframes, void *arg){

    cycles_between_frames = jack_get_sample_rate(jclient) * fpsden / (fpsnum*nframes) + 4;
    verbose("%d cycles between frames", cycles_between_frames);

    //change audio buffer
    if(audioin)
        free(audioin);
    audioin = malloc(nframes * sizeof(unsigned char));
    if(audioin==NULL)
        return 1;
    return 0;
}



int jack_graph_order(void *arg){
    jack_latency_range_t jlty;

    if(!jinput) return 0;
    jack_port_get_latency_range(jinput, JackCaptureLatency, &jlty);
    jlatency = jlty.max;
    verbose("latency: %d", jlatency);

    return 0;
}










static void usage(const char *argv0){
    printf(
        "Usage: %s [ OPTIONS ] MIDIFILE ...\n"
        "\t-h, --help                   #yolo\n"
        "\t-V, --version                current version\n"
        "\t-v,                          verbose mode\n"
        "\t-f, --fps <num>[/den]        fps\n"
        "\t-i, --input=client:port      connect jack port to intput port\n"        
        "\t-o, --ouput=client:port[,..] connect ouput to alsa midi port(s)\n",
        argv0);
}


static inline void version(void){
    puts(NAME " dev");
}


int main(int argc, char **argv){

    char *midi_outputs = NULL;
    char *jack_input = NULL;

    /* OPTIONS PARSING */
    static const struct option long_options[] = {
        {"help",    no_argument,        NULL, 'h'},
        {"version", no_argument,        NULL, 'V'},
        {"verbose", no_argument,        NULL, 'v'},
        {"fps",     required_argument,  NULL, 'f'},
        {"input",   required_argument,  NULL, 'i'},
        {"output",  required_argument,  NULL, 'o'},
    };

    int c;
    while((c = getopt_long(argc, argv, "hVvf:i:o:", long_options, NULL)) != -1){
        switch(c){
            case 'h':
                usage(argv[0]);
                return EXIT_SUCCESS;
            case 'V':
                version();
                return EXIT_SUCCESS;
            case 'v':
                doverbose=1;
                break;
            case 'f':
            {
                fpsnum = atoi(optarg);
                char *tmp = strchr(optarg, '/');
                if(tmp) fpsden=atoi(++tmp);
            }
            case 'i':
                jack_input = strdup(optarg);
                break;
            case 'o':
                midi_outputs = strdup(optarg);
                break;
            default:
                usage(argv[0]);
                return EXIT_FAILURE;

        }
    }
    if(optind >= argc){
        error("no file specified");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    fps = ceil((double) fpsnum/fpsden);



    /* START ALSA SEQ */
    if(snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0))
        fatal("cannot open alsa sequencer");
    snd_seq_set_client_name(seq, NAME);
    mclient = snd_seq_client_id(seq);

    //create port
    snd_seq_create_simple_port(seq, "output",
            SND_SEQ_PORT_CAP_READ |
            SND_SEQ_PORT_CAP_SUBS_READ
            ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC |
            SND_SEQ_PORT_TYPE_APPLICATION
    );

    if(midi_outputs){ //if midi destination defined
        snd_seq_port_subscribe_t *subs;
        snd_seq_addr_t sender, dest;
        char *s,*portname;

        snd_seq_port_subscribe_alloca(&subs);
        sender.client = mclient;
        sender.port = 0;
        snd_seq_port_subscribe_set_sender(subs,&sender);

        for(s=portname=midi_outputs; s; portname=s+1){//loop over outputs
            s = strchr(portname, ',');//ouputs separated by commas
            if(s)
                *s = '\0';//make that a eos
            if(snd_seq_parse_address(seq, &dest, portname) < 0){
                error("cannot connect to %s midi port", portname);
                continue;
            }
            snd_seq_port_subscribe_set_dest(subs,&dest);
            snd_seq_subscribe_port(seq,subs);
        }
        free(midi_outputs);
    }
        


    //create queue
    mqueue = snd_seq_alloc_named_queue(seq, NAME);

    /* READ FILE */
    if(!read_smf(argv[optind]))
        return EXIT_FAILURE;



    /* START JACK */
    jack_status_t jstatus;
    jclient = jack_client_open(NAME, JackNullOption, &jstatus);
    if(jclient == NULL){
        if(jstatus & JackServerFailed)
            error("cannot connect to JACK server");
        return EXIT_FAILURE;
    }

    if(jstatus & JackServerStarted)
        error("Started JACK server");

    //callbacks
    jack_set_process_callback (jclient, process, 0);
    jack_on_shutdown (jclient, jack_shutdown, 0);
    jack_set_buffer_size_callback (jclient, jack_bufsize, 0);
    jack_set_graph_order_callback (jclient, jack_graph_order, 0);
    

    //port
    if((jinput = jack_port_register(jclient, "input", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput | JackPortIsTerminal, 0)) == NULL)
        fatal("cannot register input port");




    /*DECODER*/
    ltcdecoder = ltc_decoder_create(jack_get_sample_rate(jclient) * fpsden / fpsnum, LTC_QUEUE_LEN);

    //we have to divide this value by the frame length to get skew
    samples_per_frame_skew = (long) jack_get_sample_rate(jclient) * fpsden * SKEW_BASE / fpsnum;
#ifdef SEND_PITCH_CONTROL
    skewpitchbias = log2(SKEW_BASE)*8192*6;//remove this value to log2(skew) to get pitch bend
#endif

    //activate jack
    if(jack_activate(jclient))
        fatal("cannot activate client");

    //jack connect
    if(jack_input){//if jack audio source defined
        if(jack_connect(jclient, jack_input, jack_port_name(jinput)))
            error("cannot %s to %s jack port", jack_input, jack_port_name(jinput));
        free(jack_input);
    }
    
    signal(SIGHUP, catchsig);
    signal(SIGINT, catchsig);



    loop();

    cleanup();
    return EXIT_SUCCESS;

}



// vim: sw=4
