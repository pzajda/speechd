
/*
 * ibmtts.c - Speech Dispatcher backend for IBM TTS
 *
 * Copyright (C) 2006 Brailcom, o.p.s.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * @author  Gary Cramblitt <garycramblitt@comcast.net> (original author)
 *
 * $Id: ibmtts.c,v 1.19 2006-04-24 23:07:51 cramblitt Exp $
 */

/* This output module operates with four threads:

        The main thread called from Speech Dispatcher (module_*()).
        A synthesis thread that accepts messages, parses them, and forwards
            them to the IBM TTS via the Eloquence Command Interface (ECI).
            This thread receives audio and index mark callbacks from
            IBM TTS and queues them into a playback queue. See _ibmtts_synth().
        A playback thread that acts on entries in the playback queue,
            either sending them to the audio output module (spd_audio_play()),
            or emitting Speech Dispatcher events.  See _ibmtts_play().
        A thread which is used to stop or pause the synthesis and
            playback threads.  See _ibmtts_stop_or_pause().

   Semaphores and mutexes are used to mediate between the 4 threads.
*/

/* System includes. */
#include <glib.h>

/* IBM Eloquence Command Interface.
   Won't exist unless IBM TTS or IBM TTS SDK is installed. */
#include <eci.h>

/* Speech Dispatcher includes. */
#include "config.h"
#include "spd_audio.h"
#include "fdset.h"
#include "module_utils.h"
#include "module_utils_addvoice.c"

#if HAVE_SNDFILE
#include <sndfile.h>
#endif

typedef enum { IBMTTS_FALSE, IBMTTS_TRUE } TIbmttsBool;
typedef enum {
    FATAL_ERROR = -1,
    OK = 0,
    ERROR = 1
} TIbmttsSuccess;

/* TODO: These defines are in src/server/index_marking.h, but including that
         file here causes a redefinition error on FATAL macro in speechd.h. */
#define SD_MARK_BODY_LEN 6
#define SD_MARK_BODY "__spd_"
#define SD_MARK_HEAD "<mark name=\""SD_MARK_BODY
#define SD_MARK_TAIL "\"/>"

#define SD_MARK_HEAD_ONLY "<mark name=\""
#define SD_MARK_HEAD_ONLY_LEN 12
#define SD_MARK_TAIL_LEN 3

#define MODULE_NAME     "ibmtts"
#define MODULE_VERSION  "0.1"

#define DEBUG_MODULE 1
DECLARE_DEBUG();

/* Define a hash table where each entry is a double-linked list
   loaded from the config file.  Each entry in the config file
   is 3 strings, where the 1st string is used to access a list
   of the 2nd and 3rd strings. */
#define MOD_OPTION_3_STR_HT_DLL(name, arg1, arg2, arg3) \
    typedef struct{ \
        char* arg2; \
        char* arg3; \
    }T ## name; \
    GHashTable *name; \
    \
    DOTCONF_CB(name ## _cb) \
    { \
        T ## name *new_item; \
        char *new_key; \
        GList *dll = NULL; \
        new_item = (T ## name *) malloc(sizeof(T ## name)); \
        new_key = strdup(cmd->data.list[0]); \
        if (NULL != cmd->data.list[1]) \
            new_item->arg2 = strdup(cmd->data.list[1]); \
        else \
            new_item->arg2 = NULL; \
        if (NULL != cmd->data.list[2]) \
            new_item->arg3 = strdup(cmd->data.list[2]); \
        else \
            new_item->arg3 = NULL; \
        dll = g_hash_table_lookup(name, new_key); \
        dll = g_list_append(dll, new_item); \
        g_hash_table_insert(name, new_key, dll); \
        return NULL; \
    }

/* Load a double-linked list from config file. */
#define MOD_OPTION_HT_DLL_REG(name) \
    name = g_hash_table_new(g_str_hash, g_str_equal); \
    module_dc_options = module_add_config_option(module_dc_options, \
                                     &module_num_dc_options, #name, \
                                     ARG_LIST, name ## _cb, NULL, 0);

/* Define a hash table mapping a string to 7 integer values. */
#define MOD_OPTION_6_INT_HT(name, arg1, arg2, arg3, arg4, arg5, arg6, arg7) \
    typedef struct{ \
        int arg1; \
        int arg2; \
        int arg3; \
        int arg4; \
        int arg5; \
        int arg6; \
        int arg7; \
    }T ## name; \
    GHashTable *name; \
    \
    DOTCONF_CB(name ## _cb) \
    { \
        T ## name *new_item; \
        char* new_key; \
        new_item = (T ## name *) malloc(sizeof(T ## name)); \
        if (cmd->data.list[0] == NULL) return NULL; \
        new_key = strdup(cmd->data.list[0]); \
        new_item->arg1 = (int) strtol(cmd->data.list[1], NULL, 10); \
        new_item->arg2 = (int) strtol(cmd->data.list[2], NULL, 10); \
        new_item->arg3 = (int) strtol(cmd->data.list[3], NULL, 10); \
        new_item->arg4 = (int) strtol(cmd->data.list[4], NULL, 10); \
        new_item->arg5 = (int) strtol(cmd->data.list[5], NULL, 10); \
        new_item->arg6 = (int) strtol(cmd->data.list[6], NULL, 10); \
        new_item->arg7 = (int) strtol(cmd->data.list[7], NULL, 10); \
        g_hash_table_insert(name, new_key, new_item); \
        return NULL; \
    }

/* Thread and process control. */
static pthread_t ibmtts_synth_thread;
static pthread_t ibmtts_play_thread;
static pthread_t ibmtts_stop_or_pause_thread;

static sem_t *ibmtts_synth_semaphore;
static sem_t *ibmtts_play_semaphore;
static sem_t *ibmtts_stop_or_pause_semaphore;

static pthread_mutex_t ibmtts_synth_suspended_mutex;
static pthread_mutex_t ibmtts_play_suspended_mutex;
static pthread_mutex_t ibmtts_stop_or_pause_suspended_mutex;

static TIbmttsBool ibmtts_thread_exit_requested = IBMTTS_FALSE;
static TIbmttsBool ibmtts_stop_synth_requested = IBMTTS_FALSE;
static TIbmttsBool ibmtts_stop_play_requested = IBMTTS_FALSE;
static TIbmttsBool ibmtts_pause_requested = IBMTTS_FALSE;

/* Current message from Speech Dispatcher. */
static char **ibmtts_message;
static EMessageType ibmtts_message_type;

/* ECI */
static ECIHand eciHandle = NULL_ECI_HAND;
static int eci_sample_rate = 0;

/* ECI sends audio back in chunks to this buffer.
   The smaller the buffer, the higher the overhead, but the better
   the index mark resolution. */
typedef signed short int TEciAudioSamples;
static TEciAudioSamples *audio_chunk;

/* For some reason, these were left out of eci.h. */
typedef enum {
    eciTextModeDefault = 0,
    eciTextModeAlphaSpell = 1,
    eciTextModeAllSpell = 2,
    eciIRCSpell = 3
} ECITextMode;

/* The playback queue. */
typedef enum {
    IBMTTS_QET_AUDIO,        /* Chunk of audio. */
    IBMTTS_QET_INDEX_MARK,   /* Index mark event. */
    IBMTTS_QET_SOUND_ICON,   /* A Sound Icon */
    IBMTTS_QET_BEGIN,        /* Beginning of speech. */
    IBMTTS_QET_END           /* Speech completed. */
} EPlaybackQueueEntryType;

typedef struct {
    long num_samples;
    TEciAudioSamples *audio_chunk;
} TPlaybackQueueAudioChunk;

typedef struct {
    EPlaybackQueueEntryType type;
    union {
        long markId;
        TPlaybackQueueAudioChunk audio;
        char *sound_icon_filename;
    } data;
} TPlaybackQueueEntry;

static GSList *playback_queue = NULL;
static pthread_mutex_t playback_queue_mutex;

/* A lookup table for index mark name given integer id. */
GHashTable *ibmtts_index_mark_ht = NULL;

/* Audio. */
AudioID *ibmtts_audio_id = NULL;
AudioOutputType ibmtts_audio_output_method;
char *ibmtts_audio_pars[10];
pthread_mutex_t sound_stop_mutex;

/* When a voice is set, this is the baseline pitch of the voice.
   SSIP PITCH commands then adjust relative to this. */
int ibmtts_voice_pitch_baseline;
/* When a voice is set, this the default speed of the voice.
   SSIP RATE commands then adjust relative to this. */
int ibmtts_voice_speed;

/* Internal function prototypes for main thread. */
static void ibmtts_set_language(char *lang);
static void ibmtts_set_voice(EVoiceType voice);
static char* ibmtts_voice_enum_to_str(EVoiceType voice);
static void ibmtts_set_language_and_voice(char *lang, EVoiceType voice);
static void ibmtts_set_rate(signed int rate);
static void ibmtts_set_pitch(signed int pitch);
static void ibmtts_set_volume(signed int pitch);

/* Internal function prototypes for synthesis thread. */
static char* ibmtts_extract_mark_name(char *mark);
static char* ibmtts_next_part(char *msg, char **mark_name);
static enum ECILanguageDialect ibmtts_dialect_to_code(char *dialect_name);
static int ibmtts_replace(char *from, char *to, GString *msg);
static void ibmtts_subst_keys_cb(gpointer data, gpointer user_data);
static char* ibmtts_subst_keys(char *key);
static char *ibmtts_search_for_sound_icon(const char *icon_name);
static TIbmttsBool ibmtts_add_sound_icon_to_playback_queue(char* filename);

static enum ECICallbackReturn eciCallback(
    ECIHand hEngine,
    enum ECIMessage msg,
    long lparam,
    void* data
);

/* Internal function prototypes for playback thread. */
static TIbmttsBool ibmtts_add_audio_to_playback_queue(TEciAudioSamples *audio_chunk,
    long num_samples);
static TIbmttsBool ibmtts_add_mark_to_playback_queue(long markId);
static TIbmttsBool ibmtts_add_flag_to_playback_queue(EPlaybackQueueEntryType type);
static void ibmtts_delete_playback_queue_entry(TPlaybackQueueEntry *playback_queue_entry);
static TIbmttsBool ibmtts_send_to_audio(TPlaybackQueueEntry *playback_queue_entry);
static TIbmttsBool ibmtts_play_file(char *filename);

/* Miscellaneous internal function prototypes. */
static TIbmttsBool is_thread_busy(pthread_mutex_t *suspended_mutex);
static void ibmtts_log_eci_error();
static void ibmtts_clear_playback_queue();

/* The synthesis thread start routine. */
static void* _ibmtts_synth(void*);
/* The playback thread start routine. */
static void* _ibmtts_play(void*);
/* The stop_or_pause start routine. */
static void* _ibmtts_stop_or_pause(void*);

/* Module configuration options. */
/* TODO: Remove these if we decide they aren't needed. */
MOD_OPTION_1_INT(IbmttsMaxChunkLength);
MOD_OPTION_1_STR(IbmttsDelimiters);

MOD_OPTION_1_STR(IbmttsAudioOutputMethod);
MOD_OPTION_1_STR(IbmttsOSSDevice);
MOD_OPTION_1_STR(IbmttsNASServer);
MOD_OPTION_1_STR(IbmttsALSADevice);

MOD_OPTION_1_INT(IbmttsAudioChunkSize);
MOD_OPTION_1_STR(IbmttsSoundIconFolder);
MOD_OPTION_2_HT(IbmttsDialect, dialect, code);
MOD_OPTION_6_INT_HT(IbmttsVoiceParameters,
    gender, breathiness, head_size, pitch_baseline, pitch_fluctuation, roughness, speed);
MOD_OPTION_3_STR_HT_DLL(IbmttsKeySubstitution, lang, key, newkey);

/* Public functions */

int
module_load(void)
{
    INIT_SETTINGS_TABLES();

    REGISTER_DEBUG();

    /* TODO: Remove these if we decide they aren't needed. */
    MOD_OPTION_1_INT_REG(IbmttsMaxChunkLength, 3000);
    MOD_OPTION_1_STR_REG(IbmttsDelimiters, "");

    MOD_OPTION_1_STR_REG(IbmttsAudioOutputMethod, "oss");
    MOD_OPTION_1_STR_REG(IbmttsOSSDevice, "/dev/dsp");
    MOD_OPTION_1_STR_REG(IbmttsNASServer, NULL);
    MOD_OPTION_1_STR_REG(IbmttsALSADevice, "default");

    MOD_OPTION_1_INT_REG(IbmttsAudioChunkSize, 20000);
    MOD_OPTION_1_STR_REG(IbmttsSoundIconFolder, "/usr/share/sounds/sound-icons/");

    /* Register voices. */
    module_register_settings_voices();

    /* Register dialects. */
    MOD_OPTION_HT_REG(IbmttsDialect);

    /* Register voice parameters */
    MOD_OPTION_HT_REG(IbmttsVoiceParameters);

    /* Register key substitutions. */
    MOD_OPTION_HT_DLL_REG(IbmttsKeySubstitution);

    return OK;
}

#define ABORT(msg) g_string_append(info, msg); \
        DBG("FATAL ERROR:", info->str); \
        *status_info = info->str; \
        g_string_free(info, 0); \
        return FATAL_ERROR;

int
module_init(char **status_info)
{
    int ret;
    char *error;
    GString *info;
    char ibmVersion[20];
    int ibm_sample_rate;

    DBG("Ibmtts: Module init().");
    INIT_INDEX_MARKING();

    *status_info = NULL;
    info = g_string_new("");
    ibmtts_thread_exit_requested = IBMTTS_FALSE;

    /* Report versions. */
    eciVersion(ibmVersion);
    DBG("Ibmtts: IBM TTS Output Module version %s, IBM TTS Engine version %s",
        MODULE_VERSION, ibmVersion);

    /* Setup IBM TTS engine. */
    DBG("Ibmtts: Creating ECI instance.");
    eciHandle = eciNew();
    if (NULL_ECI_HAND == eciHandle ) {
        DBG("Ibmtts: Could not create ECI instance.\n");
        *status_info = strdup("Could not create ECI instance. "
            "Is the IBM TTS engine installed?");
        return FATAL_ERROR;
    }

    /* Get ECI audio sample rate. */
    ibm_sample_rate = eciGetParam(eciHandle, eciSampleRate);
    switch (ibm_sample_rate) {
        case 0:
            eci_sample_rate = 8000;
            break;
        case 1:
            eci_sample_rate = 11025;
            break;
        case 2:
            eci_sample_rate = 22050;
            break;
        default:
            DBG("Ibmtts: Invalid audio sample rate returned by ECI = %i", ibm_sample_rate);
    }

    /* Allocate a chunk for ECI to return audio. */
    audio_chunk = (TEciAudioSamples *) xmalloc((IbmttsAudioChunkSize) * sizeof (TEciAudioSamples));

    DBG("Ibmtts: Registering ECI callback.");
    eciRegisterCallback(eciHandle, eciCallback, NULL);

    DBG("Ibmtts: Registering an ECI audio buffer.");
    if (!eciSetOutputBuffer(eciHandle, IbmttsAudioChunkSize, audio_chunk)) {
        DBG("Ibmtts: Error registering ECI audio buffer.");
        ibmtts_log_eci_error();
    }

    DBG("Ibmtts: Opening audio.");
    if (!strcmp(IbmttsAudioOutputMethod, "oss")){
        DBG("Ibmtts: Using OSS sound output.");
        ibmtts_audio_pars[0] = strdup(IbmttsOSSDevice);
        ibmtts_audio_pars[1] = NULL;
        ibmtts_audio_id = spd_audio_open(AUDIO_OSS, (void**) ibmtts_audio_pars, &error);
        ibmtts_audio_output_method = AUDIO_OSS;
    }
    else if (!strcmp(IbmttsAudioOutputMethod, "nas")){
        DBG("Ibmtts: Using NAS sound output.");
        ibmtts_audio_pars[0] = IbmttsNASServer;
        ibmtts_audio_pars[1] = NULL;
        ibmtts_audio_id = spd_audio_open(AUDIO_NAS, (void**) ibmtts_audio_pars, &error);
        ibmtts_audio_output_method = AUDIO_NAS;
    }
    else if (!strcmp(IbmttsAudioOutputMethod, "alsa")){
        DBG("Ibmtts: Using ALSA sound output.");
        ibmtts_audio_pars[0] = IbmttsALSADevice;
        ibmtts_audio_pars[1] = NULL;
        ibmtts_audio_id = spd_audio_open(AUDIO_ALSA, (void**) ibmtts_audio_pars, &error);
        ibmtts_audio_output_method = AUDIO_ALSA;
    }
    else{
        ABORT("Sound output method specified in configuration not supported. "
            "Please choose 'oss', 'nas', or 'alsa'.");
    }
    if (ibmtts_audio_id == NULL){
        g_string_append_printf(info, "Opening sound device failed. Reason: %s. ", error);
        ABORT("Can't open sound device.");
    }

    /* These mutexes are locked when the corresponding threads are suspended. */
    pthread_mutex_init(&ibmtts_synth_suspended_mutex, NULL);
    pthread_mutex_init(&ibmtts_play_suspended_mutex, NULL);

    /* This mutex is used to hold a stop request until audio actually stops. */
    pthread_mutex_init(&sound_stop_mutex, NULL);

    /* This mutex mediates access to the playback queue between the synthesis and
       playback threads. */
    pthread_mutex_init(&playback_queue_mutex, NULL);

    /*
    DBG("Ibmtts: IbmttsMaxChunkLength = %d", IbmttsMaxChunkLength);
    DBG("Ibmtts: IbmttsDelimiters = %s", IbmttsDelimiters);
    */
    DBG("Ibmtts: ImbttsAudioChunkSize = %d", IbmttsAudioChunkSize);

    ibmtts_message = xmalloc (sizeof (char*));
    *ibmtts_message = NULL;

    DBG("Ibmtts: Creating new thread for stop or pause.");
    ibmtts_stop_or_pause_semaphore = module_semaphore_init();
    ret = pthread_create(&ibmtts_stop_or_pause_thread, NULL, _ibmtts_stop_or_pause, NULL);
    if(0 != ret) {
        DBG("Ibmtts: stop or pause thread creation failed.");
        *status_info = strdup("The module couldn't initialize stop or pause thread. "
            "This could be either an internal problem or an "
            "architecture problem. If you are sure your architecture "
            "supports threads, please report a bug.");
        return FATAL_ERROR;
    }

    DBG("Ibmtts: Creating new thread for playback.");
    ibmtts_play_semaphore = module_semaphore_init();
    ret = pthread_create(&ibmtts_play_thread, NULL, _ibmtts_play, NULL);
    if(0 != ret) {
        DBG("Ibmtts: play thread creation failed.");
        *status_info = strdup("The module couldn't initialize play thread. "
            "This could be either an internal problem or an "
            "architecture problem. If you are sure your architecture "
            "supports threads, please report a bug.");
        return FATAL_ERROR;
    }

    DBG("Ibmtts: Creating new thread for IBM TTS synthesis.");
    ibmtts_synth_semaphore = module_semaphore_init();
    ret = pthread_create(&ibmtts_synth_thread, NULL, _ibmtts_synth, NULL);
    if(0 != ret) {
        DBG("Ibmtts: synthesis thread creation failed.");
        *status_info = strdup("The module couldn't initialize synthesis thread. "
            "This could be either an internal problem or an "
            "architecture problem. If you are sure your architecture "
            "supports threads, please report a bug.");
        return FATAL_ERROR;
    }

    *status_info = strdup("Ibmtts: Initialized successfully.");

    return OK;
}
#undef ABORT

int
module_speak(gchar *data, size_t bytes, EMessageType msgtype)
{
    DBG("Ibmtts: module_speak().");

    if (is_thread_busy(&ibmtts_synth_suspended_mutex) || 
        is_thread_busy(&ibmtts_play_suspended_mutex) ||
        is_thread_busy(&ibmtts_stop_or_pause_suspended_mutex)) {
        DBG("Ibmtts: Already synthesizing when requested to synthesize (module_speak).");
        return IBMTTS_FALSE;
    }

    if (0 != module_write_data_ok(data)) return FATAL_ERROR;

    DBG("Ibmtts: Requested data: |%s|\n", data);

    xfree(*ibmtts_message);
    *ibmtts_message = NULL;

    *ibmtts_message = strdup(data);
    ibmtts_message_type = msgtype;
    if ((msgtype == MSGTYPE_TEXT) && (msg_settings.spelling_mode == SPELLING_ON))
        ibmtts_message_type = MSGTYPE_SPELL;

    /* Setting speech parameters. */
    UPDATE_STRING_PARAMETER(language, ibmtts_set_language);
    UPDATE_PARAMETER(voice, ibmtts_set_voice);
    UPDATE_PARAMETER(rate, ibmtts_set_rate);
    UPDATE_PARAMETER(volume, ibmtts_set_volume);
    UPDATE_PARAMETER(pitch, ibmtts_set_pitch);
    /* TODO: Handle these in _ibmtts_synth() ?
    UPDATE_PARAMETER(punctuation_mode, festival_set_punctuation_mode);
    UPDATE_PARAMETER(cap_let_recogn, festival_set_cap_let_recogn);
    */

    /* Send semaphore signal to the synthesis thread */
    sem_post(ibmtts_synth_semaphore);

    DBG("Ibmtts: Leaving module_speak() normally.");
    return bytes;
}

int
module_stop(void)
{
    DBG("Ibmtts: module_stop().");

    /* Request both synth and playback threads to stop what they are doing
       (if anything). */
    ibmtts_stop_synth_requested = IBMTTS_TRUE;
    ibmtts_stop_play_requested = IBMTTS_TRUE;

    /* Wake the stop_or_pause thread. */
    sem_post(ibmtts_stop_or_pause_semaphore);

    return OK;
}

size_t
module_pause(void)
{
    /* The semantics of module_pause() is the same as module_stop()
       except that processing should continue until the next index mark is
       reached before stopping.
       Note that although IBM TTS offers an eciPause function, we cannot
       make use of it because Speech Dispatcher doesn't have a module_resume
       function. Instead, Speech Dispatcher resumes by calling module_speak
       from the last index mark reported in the text. */
    DBG("Ibmtts: module_pause().");

    /* Request playback thread to pause.  Note we cannot stop synthesis or
       playback until end of sentence or end of message is played. */
    ibmtts_pause_requested = IBMTTS_TRUE;

    /* Wake the stop_or_pause thread. */
    sem_post(ibmtts_stop_or_pause_semaphore);

    return OK;
}

void
module_close(int status)
{

    DBG("Ibmtts: close().");

    if(is_thread_busy(&ibmtts_synth_suspended_mutex) ||
        is_thread_busy(&ibmtts_play_suspended_mutex)) {
        DBG("Ibmtts: Stopping speech");
        module_stop();
    }

    DBG("Ibmtts: De-registering ECI callback.");
    eciRegisterCallback(eciHandle, NULL, NULL);

    DBG("Ibmtts: Destroying ECI instance.");
    eciDelete(eciHandle);
    eciHandle = NULL_ECI_HAND;

    /* Free buffer for ECI audio. */
    xfree(audio_chunk);

    DBG("Ibmtts: Closing audio output");
    spd_audio_close(ibmtts_audio_id);

    /* Request each thread exit and wait until it exits. */
    DBG("Ibmtts: Terminating threads");
    ibmtts_thread_exit_requested = IBMTTS_TRUE;
    sem_post(ibmtts_synth_semaphore);
    sem_post(ibmtts_play_semaphore);
    sem_post(ibmtts_stop_or_pause_semaphore);
    if (0 != pthread_join(ibmtts_synth_thread, NULL))
        exit(1);
    if (0 != pthread_join(ibmtts_play_thread, NULL))
        exit(1);
    if (0 != pthread_join(ibmtts_stop_or_pause_thread, NULL))
        exit(1);

    ibmtts_clear_playback_queue();

    /* Free index mark lookup table. */
    if (ibmtts_index_mark_ht) {
        g_hash_table_destroy(ibmtts_index_mark_ht);
        ibmtts_index_mark_ht = NULL;
    }

    exit(status);
}

/* Internal functions */

/* Return true if the thread is busy, i.e., suspended mutex is not locked. */
static TIbmttsBool
is_thread_busy(pthread_mutex_t *suspended_mutex)
{
    if (EBUSY == pthread_mutex_trylock(suspended_mutex))
        return IBMTTS_FALSE;
    else
    {
        pthread_mutex_unlock(suspended_mutex);
        return IBMTTS_TRUE;
    }
}

/* Given a string containing an index mark in the form
   <mark name="some_name"/>, returns some_name.  Calling routine is
   responsible for freeing returned string. If an error occurs,
   returns NULL. */
static char*
ibmtts_extract_mark_name(char *mark)
{
    if ((SD_MARK_HEAD_ONLY_LEN + SD_MARK_TAIL_LEN + 1) > strlen(mark)) return NULL;
    mark = mark + SD_MARK_HEAD_ONLY_LEN;
    char* tail = strstr(mark, SD_MARK_TAIL);
    if (NULL == tail) return NULL;
    return (char *) strndup(mark, tail - mark);
}

/* Returns the portion of msg up to, but not including, the next index
   mark, or end of msg if no index mark is found.  If msg begins with
   and index mark, returns the entire index mark clause (<mark name="whatever"/>)
   and returns the mark name.  If msg does not begin with an index mark,
   mark_name will be NULL. If msg is empty, returns a zero-length string (not NULL).
   Caller is responsible for freeing both returned string and mark_name (if not NULL). */
/* TODO: This routine needs to be more tolerant of custom index marks with spaces. */
/* TODO: Should there be a MaxChunkLength? Delimiters? */
static char*
ibmtts_next_part(char *msg, char **mark_name)
{
    char* mark_head = strstr(msg, SD_MARK_HEAD_ONLY);
    if (NULL == mark_head)
        return (char *) strndup(msg, strlen(msg));
    else if (mark_head == msg) {
        *mark_name = ibmtts_extract_mark_name(mark_head);
        if (NULL == *mark_name)
            return strcat((char *) strndup(msg, SD_MARK_HEAD_ONLY_LEN),
                ibmtts_next_part(msg + SD_MARK_HEAD_ONLY_LEN, mark_name));
        else
            return (char *) strndup(msg, SD_MARK_HEAD_ONLY_LEN + strlen(*mark_name) +
                SD_MARK_TAIL_LEN);
    } else
        return (char *) strndup(msg, mark_head - msg);
}

/* Stop or Pause thread. */
static void*
_ibmtts_stop_or_pause(void* nothing)
{
    DBG("Ibmtts: Stop or pause thread starting.......\n");

    /* Block all signals to this thread. */
    set_speaking_thread_parameters();

    while (!ibmtts_thread_exit_requested)
    {
        /* If semaphore not set, set suspended lock and suspend until it is signaled. */
        if (0 != sem_trywait(ibmtts_stop_or_pause_semaphore))
        {
            pthread_mutex_lock(&ibmtts_stop_or_pause_suspended_mutex);
            sem_wait(ibmtts_stop_or_pause_semaphore);
            pthread_mutex_unlock(&ibmtts_stop_or_pause_suspended_mutex);
            if (ibmtts_thread_exit_requested) break;
        }
        DBG("Ibmtts: Stop or pause semaphore on.");

        if (ibmtts_stop_synth_requested) {
            /* Stop synthesis (if in progress). */
            if (eciHandle)
            {
                DBG("Ibmtts: Stopping synthesis.");
                eciStop(eciHandle);
            }

            /* Stop any audio playback (if in progress). */
            if (ibmtts_audio_id)
            {
                pthread_mutex_lock(&sound_stop_mutex);
                DBG("Ibmtts: Stopping audio.");
                int ret = spd_audio_stop(ibmtts_audio_id);
                if (0 != ret) DBG("Ibmtts: WARNING: Non 0 value from spd_audio_stop: %d", ret);
                pthread_mutex_unlock(&sound_stop_mutex);
            }
        }

        DBG("Ibmtts: Waiting for synthesis thread to suspend.");
        while (is_thread_busy(&ibmtts_synth_suspended_mutex))
            g_usleep(100);
        DBG("Ibmtts: Waiting for playback thread to suspend.");
        while (is_thread_busy(&ibmtts_play_suspended_mutex))
            g_usleep(100);

        DBG("Ibmtts: Clearing playback queue.");
        ibmtts_clear_playback_queue();

        DBG("Ibmtts: Clearing index mark lookup table.");
        if (ibmtts_index_mark_ht) {
            g_hash_table_destroy(ibmtts_index_mark_ht);
            ibmtts_index_mark_ht = NULL;
        }

        if (ibmtts_stop_synth_requested)
            module_report_event_stop();
        else
            module_report_event_pause();

        ibmtts_stop_synth_requested = IBMTTS_FALSE;
        ibmtts_stop_play_requested = IBMTTS_FALSE;
        ibmtts_pause_requested = IBMTTS_FALSE;

        DBG("Ibmtts: Stop or pause completed.");
    }
    DBG("Ibmtts: Stop or pause thread ended.......\n");

    pthread_exit(NULL);
}

/* Synthesis thread. */
static void*
_ibmtts_synth(void* nothing)
{
    char *pos = NULL;
    char *part = NULL;
    int part_len = 0;
    int *markId = NULL;
    TIbmttsBool scan_msg;

    DBG("Ibmtts: Synthesis thread starting.......\n");

    /* Block all signals to this thread. */
    set_speaking_thread_parameters();

    /* Allocate a place for index mark names to be placed. */
    char **mark_name = (char **) xmalloc(sizeof (char *));
    *mark_name = NULL;

    while (!ibmtts_thread_exit_requested)
    {
        /* If semaphore not set, set suspended lock and suspend until it is signaled. */
        if (0 != sem_trywait(ibmtts_synth_semaphore))
        {
            pthread_mutex_lock(&ibmtts_synth_suspended_mutex);
            sem_wait(ibmtts_synth_semaphore);
            pthread_mutex_unlock(&ibmtts_synth_suspended_mutex);
            if (ibmtts_thread_exit_requested) break;
        }
        DBG("Ibmtts: Synthesis semaphore on.");

        /* This table assigns each index mark name an integer id for fast lookup when
           ECI returns the integer index mark event. */
        if (ibmtts_index_mark_ht)
            g_hash_table_destroy(ibmtts_index_mark_ht);
        ibmtts_index_mark_ht = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, g_free);

        pos = *ibmtts_message;
        scan_msg = IBMTTS_TRUE;

        switch (ibmtts_message_type) {
            case MSGTYPE_TEXT:
                eciSetParam(eciHandle, eciTextMode, eciTextModeDefault);
                break;
            case MSGTYPE_SOUND_ICON:
                /* IBM TTS does not support sound icons.
                   If we can find a sound icon file, play that,
                   otherwise speak the name of the sound icon. */
                part = ibmtts_search_for_sound_icon(*ibmtts_message);
                if (NULL != part) {
                    ibmtts_add_flag_to_playback_queue(IBMTTS_QET_BEGIN);
                    ibmtts_add_sound_icon_to_playback_queue(part);
                    part = NULL;
                    ibmtts_add_flag_to_playback_queue(IBMTTS_QET_END);
                    scan_msg = IBMTTS_FALSE;
                    /* Wake up the audio playback thread, if not already awake. */
                    if (!is_thread_busy(&ibmtts_play_suspended_mutex))
                        sem_post(ibmtts_play_semaphore);
                } else
                    eciSetParam(eciHandle, eciTextMode, eciTextModeDefault);
                break;
            case MSGTYPE_CHAR:
                eciSetParam(eciHandle, eciTextMode, eciTextModeAllSpell);
                break;
            case MSGTYPE_KEY:
                /* Map unspeakable keys to speakable words. */
                DBG("Ibmtts: Key from Speech Dispatcher: |%s|", pos);
                pos = ibmtts_subst_keys(pos);
                DBG("Ibmtts: Key to speak: |%s|", pos);
                xfree(*ibmtts_message);
                *ibmtts_message = pos;
                eciSetParam(eciHandle, eciTextMode, eciTextModeDefault);
                break;
            case MSGTYPE_SPELL:
                if (PUNCT_NONE != msg_settings.punctuation_mode)
                    eciSetParam(eciHandle, eciTextMode, eciTextModeAllSpell);
                else
                    eciSetParam(eciHandle, eciTextMode, eciTextModeAlphaSpell);
                break;
        }

        if (0 == strlen(pos)) scan_msg = IBMTTS_FALSE;

        /* If an empty message is sent, this routine receives a buffer containing:
           <speak></speak>.  If this string is sent to IBM TTS, it hangs. 
           Actually, what is happening is no audio callback occurs, so
           this output module waits forever for something that never comes. */
        if (scan_msg)
            if (0 == strncmp("<speak></speak>", pos, 15)) scan_msg = IBMTTS_FALSE;

        if (scan_msg) ibmtts_add_flag_to_playback_queue(IBMTTS_QET_BEGIN);
        while (scan_msg) {
            if (ibmtts_stop_synth_requested) {
                DBG("Ibmtts: Stop in synthesis thread, terminating.");
                break;
            }

            /* TODO: How to map these msg_settings to ibm tts?
                EPunctMode punctuation_mode;
                    PUNCT_NONE = 0,
                    PUNCT_ALL = 1,
                    PUNCT_SOME = 2
                ESpellMode spelling_mode;
                    SPELLING_ON already handled in module_speak()
                ECapLetRecogn cap_let_recogn;
                    RECOGN_NONE = 0,
                    RECOGN_SPELL = 1,
                    RECOGN_ICON = 2
            */

            part = ibmtts_next_part(pos, mark_name);
            if (NULL == part) {
                DBG("Ibmtts: Error getting next part of message.");
                /* TODO: What to do here? */
                break;
            } else {
                part_len = strlen(part);
                pos += part_len;
            }

            /* Handle index marks. */
            if (NULL != *mark_name) {
                /* Assign the mark name an integer number and store in lookup table. */
                markId = (int *) xmalloc( sizeof (int) );
                *markId = g_hash_table_size(ibmtts_index_mark_ht);
                g_hash_table_insert(ibmtts_index_mark_ht, markId, strdup(*mark_name));
                if (!eciInsertIndex(eciHandle, *markId)) {
                    DBG("Ibmtts: Error sending index mark to synthesizer.");
                    ibmtts_log_eci_error();
                    /* Try to keep going. */
                } else
                    DBG("Ibmtts: Index mark |%s| (id %i) sent to synthesizer.",*mark_name, *markId);
                xfree(*mark_name);
                *mark_name = NULL;
                /* If pause is requested, skip over rest of message,
                   but synthesize what we have so far. */
                if (ibmtts_pause_requested) {
                    DBG("Ibmtts: Pause requested in synthesis thread.");
                    pos += strlen(pos);
                }
            }
            /* Handle normal text. */
            else if (part_len > 0) {
                DBG("Ibmtts: Returned %d bytes from get_part.", part_len);
                DBG("Ibmtts: Text to synthesize is |%s|\n", part);
                DBG("Ibmtts: Sending text to synthesizer.");
                if (!eciAddText(eciHandle, part)) {
                    DBG("Ibmtts: Error sending text.");
                    ibmtts_log_eci_error();
                    break;
                }
                xfree(part);
                part = NULL;
            }
            /* Handle end of text. */
            else {
                xfree(part);
                part = NULL;
                DBG("Ibmtts: End of data in synthesis thread.");
                DBG("Ibmtts: Trying to synthesize text.");
                if (!eciSynthesize(eciHandle)) {
                    DBG("Ibmtts: Error synthesizing.");
                    ibmtts_log_eci_error();
                    break;
                }
                /* Audio and index marks are returned in eciCallback(). */
                DBG("Ibmtts: Waiting for synthesis to complete.");
                if (!eciSynchronize(eciHandle)) {
                    DBG("Ibmtts: Error waiting for synthesis to complete.");
                    ibmtts_log_eci_error();
                    break;
                }
                ibmtts_add_flag_to_playback_queue(IBMTTS_QET_END);
                break;
            }

            if (ibmtts_stop_synth_requested){
                DBG("Ibmtts: Stop in synthesis thread, terminating.");
                break;
            }
        }
    }

    xfree(part);
    xfree(*mark_name);
    xfree(mark_name);

    DBG("Ibmtts: Synthesis thread ended.......\n");

    pthread_exit(NULL);
}

static void
ibmtts_set_rate(signed int rate)
{
    /* Setting rate to midpoint is too fast.  An eci value of 50 is "normal".
       See chart on pg 38 of the ECI manual. */
    assert(rate >= -100 && rate <= +100);
    int speed;
    /* Possible ECI range is 0 to 250. */
    /* Map -100 to 100 onto 0 to 100. */
    if (rate < 0)
        /* Map -100 to 0 onto 0 to ibmtts_voice_speed */
        speed = ((float)(rate + 100) * ibmtts_voice_speed) / (float)100;
    else
        /* Map 0 to 100 onto ibmtts_voice_speed to 100 */
        speed = (((float)rate * (100 - ibmtts_voice_speed)) / (float)100)
            + ibmtts_voice_speed;
    /* speed = (rate + 100) / 2; */
    assert(speed >= 0 && speed <= 100);
    int ret = eciSetVoiceParam(eciHandle, 0, eciSpeed, speed);
    if (-1 == ret) {
        DBG("Ibmtts: Error setting rate %i.", speed);
        ibmtts_log_eci_error();
    }
    else
        DBG("Ibmtts: Rate set to %i.", speed);
}

static void
ibmtts_set_volume(signed int volume)
{
    /* Setting volume to midpoint makes speech too soft.  An eci value
       of 90 to 100 is "normal".
       See chart on pg 38 of the ECI manual.
       TODO: Rather than setting volume in the synth, maybe control volume on playback? */
    assert(volume >= -100 && volume <= +100);
    int vol;
    /* Possible ECI range is 0 to 100. */
    if (volume < 0)
        /* Map -100 to 0 onto 0 to 90 */
        vol = (((float)volume + 100) * 90) / (float)100;
    else
        /* Map 0 to 100 onto 90 to 100 */
        vol = ((float)(volume * 10) / (float)100) + 90;
    assert(vol >= 0 && vol <= 100);
    int ret = eciSetVoiceParam(eciHandle, 0, eciVolume, vol);
    if (-1 == ret) {
        DBG("Ibmtts: Error setting volume %i.", vol);
        ibmtts_log_eci_error();
    }
    else
        DBG("Ibmtts: Volume set to %i.", vol);
}

static void
ibmtts_set_pitch(signed int pitch)
{
    /* Setting pitch to midpoint is to low.  eci values between 65 and 89
       are "normal".
       See chart on pg 38 of the ECI manual. */
    assert(pitch >= -100 && pitch <= +100);
    int pitchBaseline;
    /* Possible range 0 to 100. */
    if (pitch < 0)
        /* Map -100 to 0 onto 0 to ibmtts_voice_pitch_baseline */
        pitchBaseline = ((float)(pitch + 100) * ibmtts_voice_pitch_baseline) / (float)100;
    else
        /* Map 0 to 100 onto ibmtts_voice_pitch_baseline to 100 */
        pitchBaseline = (((float)pitch * (100 - ibmtts_voice_pitch_baseline)) / (float)100)
            + ibmtts_voice_pitch_baseline;
    assert (pitchBaseline >= 0 && pitchBaseline <= 100);
    int ret = eciSetVoiceParam(eciHandle, 0, eciPitchBaseline, pitchBaseline);
    if (-1 == ret) {
        DBG("Ibmtts: Error setting pitch %i.", pitchBaseline);
        ibmtts_log_eci_error();
    }
    else
        DBG("Ibmtts: Pitch set to %i.", pitchBaseline);
}

/* Given an IBM TTS Dialect Name returns the code from the DIALECTS table in config file. */
static enum ECILanguageDialect ibmtts_dialect_to_code(char *dialect_name)
{
    long int code = 0;
    TIbmttsDialect *dialect = (TIbmttsDialect *) module_get_ht_option(IbmttsDialect, dialect_name);
    if (NULL == dialect) {
        DBG("Ibmtts: Invalid dialect name %s.  Check VOICES and DIALECTS sections of ibmtts.conf file.", dialect_name);
        ibmtts_log_eci_error();
        return NODEFINEDCODESET;
    }
    sscanf(dialect->code, "%08X", &code);
    return code;
}

static char*
ibmtts_voice_enum_to_str(EVoiceType voice)
{
    /* TODO: Would be better to move this to module_utils.c. */
    char *voicename;
    switch (voice) {
        case NO_VOICE:      voicename = strdup("no voice");     break;
        case MALE1:         voicename = strdup("male1");        break;
        case MALE2:         voicename = strdup("male2");        break;
        case MALE3:         voicename = strdup("male3");        break;
        case FEMALE1:       voicename = strdup("female1");      break;
        case FEMALE2:       voicename = strdup("female2");      break;
        case FEMALE3:       voicename = strdup("female3");      break;
        case CHILD_MALE:    voicename = strdup("child_male");   break;
        case CHILD_FEMALE:  voicename = strdup("child_female"); break;
        default:            voicename = strdup("no voice");     break;
    }
    return voicename;
}

/* Given a language code and SD voice code, sets the IBM voice dialect based
   upon VOICES table in config file. */
static void
ibmtts_set_language_and_voice(char *lang, EVoiceType voice)
{
    char *dialect_name;
    enum ECILanguageDialect dialect_code = NODEFINEDCODESET;
    char *voicename = ibmtts_voice_enum_to_str(voice);
    int eciVoice;
    int ret = -1;
    /* Map language and symbolic voice name to dialect name. */
    dialect_name = module_getvoice(lang, voice);
    if (NULL != dialect_name) {
         /* Map dialect name to dialect code. */
         dialect_code = ibmtts_dialect_to_code(dialect_name);
         if (NODEFINEDCODESET != dialect_code) {
             /* Set the dialect. */
             ret = eciSetParam(eciHandle, eciLanguageDialect, dialect_code);
        }
    }
    if (-1 == ret) {
        DBG("Ibmtts: Unable to set language %s and voice %s,  dialect %08X.",
            lang, voicename, dialect_code);
        ibmtts_log_eci_error();
    } else
        DBG("Ibmtts: Successfully set language %s and voice %s, dialect %s = %08X.",
            lang, voicename, dialect_name, dialect_code);
    /* Set voice parameters (if any are defined for this voice.) */
    TIbmttsVoiceParameters *params = g_hash_table_lookup(IbmttsVoiceParameters, voicename);
    if (NULL == params) {
        DBG("Ibmtts: Setting default VoiceParameters for voice %s", voicename);
        switch (voice) {
            case MALE1:         eciVoice = 1;   break;  /* Adult Male 1 */
            case MALE2:         eciVoice = 4;   break;  /* Adult Male 2 */
            case MALE3:         eciVoice = 5;   break;  /* Adult Male 3 */
            case FEMALE1:       eciVoice = 2;   break;  /* Adult Female 1 */
            case FEMALE2:       eciVoice = 6;   break;  /* Adult Female 2 */
            case FEMALE3:       eciVoice = 7;   break;  /* Elderly Female 1 */
            case CHILD_MALE:    eciVoice = 3;   break;  /* Child */
            case CHILD_FEMALE:  eciVoice = 3;   break;  /* Child */
            default:            eciVoice = 1;   break;  /* Adult Male 1 */
        }
        ret = eciCopyVoice(eciHandle, eciVoice, 0);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting default voice parameters (voice %i).",
            eciVoice);
    } else {
        DBG("Ibmtts: Setting custom VoiceParameters for voice %s", voicename);
        ret = eciSetVoiceParam(eciHandle, 0, eciGender, params->gender);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting gender %i", params->gender);
        ret = eciSetVoiceParam(eciHandle, 0, eciBreathiness, params->breathiness);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting breathiness %i", params->breathiness);
        ret = eciSetVoiceParam(eciHandle, 0, eciHeadSize, params->head_size);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting head size %i", params->head_size);
        ret = eciSetVoiceParam(eciHandle, 0, eciPitchBaseline, params->pitch_baseline);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting pitch baseline %i", params->pitch_baseline);
        ret = eciSetVoiceParam(eciHandle, 0, eciPitchFluctuation, params->pitch_fluctuation);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting pitch fluctuation %i", params->pitch_fluctuation);
        ret = eciSetVoiceParam(eciHandle, 0, eciRoughness, params->roughness);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting roughness %i", params->roughness);
        ret = eciSetVoiceParam(eciHandle, 0, eciSpeed, params->speed);
        if (-1 == ret) DBG("Ibmtts: ERROR: Setting speed %i", params->speed);
    }
    xfree(voicename);
    /* Retrieve the baseline pitch and speed of the voice. */
    ibmtts_voice_pitch_baseline = eciGetVoiceParam(eciHandle, 0, eciPitchBaseline);
    if (-1 == ibmtts_voice_pitch_baseline) DBG("Ibmtts: Cannot get pitch baseline of voice.");
    ibmtts_voice_speed = eciGetVoiceParam(eciHandle, 0, eciSpeed);
    if (-1 == ibmtts_voice_speed) DBG("Ibmtts: Cannot get speed of voice.");
}

static void
ibmtts_set_voice(EVoiceType voice)
{
    assert(msg_settings.language);
    ibmtts_set_language_and_voice(msg_settings.language, voice);
}

static void
ibmtts_set_language(char *lang)
{
    assert(msg_settings.voice);
    ibmtts_set_language_and_voice(lang, msg_settings.voice);
}

static void
ibmtts_log_eci_error()
{
    /* TODO: This routine is not working.  Not sure why. */
    char buf[100];
    eciErrorMessage(eciHandle, buf);
    DBG("Ibmtts: ECI Error Message: %s", buf);
}

/* IBM TTS calls back here when a chunk of audio is ready or an index mark
   has been reached.  The good news is that it returns the audio up to
   each index mark or when the audio buffer is full. */
static enum ECICallbackReturn eciCallback(
    ECIHand hEngine,
    enum ECIMessage msg,
    long lparam,
    void* data )
{
    int ret;
    /* This callback is running in the same thread as called eciSynchronize(),
       i.e., the _ibmtts_synth() thread. */

    /* If module_stop was called, discard any further callbacks until module_speak is called. */
    if (ibmtts_stop_synth_requested || ibmtts_stop_play_requested) return eciDataProcessed;

    switch (msg) {
        case eciWaveformBuffer:
            DBG("Ibmtts: %d audio samples returned from IBM TTS.", lparam);
            /* Add audio to output queue. */
            ret = ibmtts_add_audio_to_playback_queue(audio_chunk, lparam);
            /* Wake up the audio playback thread, if not already awake. */
            if (!is_thread_busy(&ibmtts_play_suspended_mutex))
                sem_post(ibmtts_play_semaphore);
            return eciDataProcessed;
            break;
        case eciIndexReply:
            DBG("Ibmtts: Index mark id %d returned from IBM TTS.", lparam);
            /* Add index mark to output queue. */
            ret = ibmtts_add_mark_to_playback_queue(lparam);
            /* Wake up the audio playback thread, if not already awake. */
            if (!is_thread_busy(&ibmtts_play_suspended_mutex))
                sem_post(ibmtts_play_semaphore);
            return eciDataProcessed;
            break;
    }
}

/* Adds a chunk of pcm audio to the audio playback queue. */
static TIbmttsBool
ibmtts_add_audio_to_playback_queue(TEciAudioSamples *audio_chunk, long num_samples)
{
    TPlaybackQueueEntry *playback_queue_entry = (TPlaybackQueueEntry *) xmalloc (sizeof (TPlaybackQueueEntry));
    if (NULL == playback_queue_entry) return IBMTTS_FALSE;
    playback_queue_entry->type = IBMTTS_QET_AUDIO;
    playback_queue_entry->data.audio.num_samples = (int) num_samples;
    int wlen = sizeof (TEciAudioSamples) * num_samples;
    playback_queue_entry->data.audio.audio_chunk = (TEciAudioSamples *) xmalloc (wlen);
    memcpy(playback_queue_entry->data.audio.audio_chunk, audio_chunk, wlen);
    pthread_mutex_lock(&playback_queue_mutex);
    playback_queue = g_slist_append(playback_queue, playback_queue_entry);
    pthread_mutex_unlock(&playback_queue_mutex);
    return IBMTTS_TRUE;
}

/* Adds an Index Mark to the audio playback queue. */
static TIbmttsBool
ibmtts_add_mark_to_playback_queue(long markId)
{
    TPlaybackQueueEntry *playback_queue_entry = (TPlaybackQueueEntry *) xmalloc (sizeof (TPlaybackQueueEntry));
    if (NULL == playback_queue_entry) return IBMTTS_FALSE;
    playback_queue_entry->type = IBMTTS_QET_INDEX_MARK;
    playback_queue_entry->data.markId = markId;
    pthread_mutex_lock(&playback_queue_mutex);
    playback_queue = g_slist_append(playback_queue, playback_queue_entry);
    pthread_mutex_unlock(&playback_queue_mutex);
    return IBMTTS_TRUE;
}

/* Adds a begin or end flag to the playback queue. */
static TIbmttsBool
ibmtts_add_flag_to_playback_queue(EPlaybackQueueEntryType type)
{
    TPlaybackQueueEntry *playback_queue_entry = (TPlaybackQueueEntry *) xmalloc (sizeof (TPlaybackQueueEntry));
    if (NULL == playback_queue_entry) return IBMTTS_FALSE;
    playback_queue_entry->type = type;
    pthread_mutex_lock(&playback_queue_mutex);
    playback_queue = g_slist_append(playback_queue, playback_queue_entry);
    pthread_mutex_unlock(&playback_queue_mutex);
    return IBMTTS_TRUE;
}

/* Add a sound icon to the playback queue. */
static TIbmttsBool
ibmtts_add_sound_icon_to_playback_queue(char* filename)
{
    TPlaybackQueueEntry *playback_queue_entry = (TPlaybackQueueEntry *) xmalloc (sizeof (TPlaybackQueueEntry));
    if (NULL == playback_queue_entry) return IBMTTS_FALSE;
    playback_queue_entry->type = IBMTTS_QET_SOUND_ICON;
    playback_queue_entry->data.sound_icon_filename = filename;
    pthread_mutex_lock(&playback_queue_mutex);
    playback_queue = g_slist_append(playback_queue, playback_queue_entry);
    pthread_mutex_unlock(&playback_queue_mutex);
    return IBMTTS_TRUE;
}

/* Deletes an entry from the playback audio queue, freeing memory. */
static void
ibmtts_delete_playback_queue_entry(TPlaybackQueueEntry *playback_queue_entry)
{
    switch (playback_queue_entry->type) {
        case IBMTTS_QET_AUDIO:
            xfree(playback_queue_entry->data.audio.audio_chunk);
            break;
        case IBMTTS_QET_SOUND_ICON:
            xfree(playback_queue_entry->data.sound_icon_filename);
            break;
        default:
            break;
    }
    xfree(playback_queue_entry);
}

/* Erases the entire playback queue, freeing memory. */
static void
ibmtts_clear_playback_queue()
{
    pthread_mutex_lock(&playback_queue_mutex);
    while (NULL != playback_queue)
    {
        TPlaybackQueueEntry *playback_queue_entry = playback_queue->data;
        ibmtts_delete_playback_queue_entry(playback_queue_entry);
        playback_queue = g_slist_remove(playback_queue, playback_queue->data);
    }
    playback_queue = NULL;
    pthread_mutex_unlock(&playback_queue_mutex);
}

/* Sends a chunk of audio to the audio player and waits for completion or error. */
static TIbmttsBool
ibmtts_send_to_audio(TPlaybackQueueEntry *playback_queue_entry)
{
    AudioTrack track;
    track.num_samples = playback_queue_entry->data.audio.num_samples;
    track.num_channels = 1;
    track.sample_rate = eci_sample_rate;
    track.bits = 16;
    track.samples = playback_queue_entry->data.audio.audio_chunk;

    if (track.samples != NULL){
        DBG("Ibmtts: Sending %i samples to audio.", track.num_samples);
        /* Volume is controlled by the synthesizer.  Always play at normal on audio device. */
        spd_audio_set_volume(ibmtts_audio_id, 0);
        int ret = spd_audio_play(ibmtts_audio_id, track);
        if (ret < 0) {
            DBG("ERROR: Can't play track for unknown reason.");
            return IBMTTS_FALSE;
        }
        DBG("Ibmtts: Sent to audio.");
    }

    return IBMTTS_TRUE;
}

/* Playback thread. */
static void*
_ibmtts_play(void* nothing)
{
    int markId;
    char *mark_name;
    TPlaybackQueueEntry *playback_queue_entry = NULL;

    DBG("Ibmtts: Playback thread starting.......\n");

    /* Block all signals to this thread. */
    set_speaking_thread_parameters();

    while (!ibmtts_thread_exit_requested)
    {
        /* If semaphore not set, set suspended lock and suspend until it is signaled. */
        if (0 != sem_trywait(ibmtts_play_semaphore))
        {
            pthread_mutex_lock(&ibmtts_play_suspended_mutex);
            sem_wait(ibmtts_play_semaphore);
            pthread_mutex_unlock(&ibmtts_play_suspended_mutex);
            if (ibmtts_thread_exit_requested) break;
        }
        /* DBG("Ibmtts: Playback semaphore on."); */

        pthread_mutex_lock(&playback_queue_mutex);
        if (NULL != playback_queue) {
            playback_queue_entry = playback_queue->data;
            playback_queue = g_slist_remove(playback_queue, playback_queue->data);
        }
        pthread_mutex_unlock(&playback_queue_mutex);

        while ((NULL != playback_queue_entry) && !ibmtts_stop_play_requested)
        {
            switch (playback_queue_entry->type) {
                case IBMTTS_QET_AUDIO:
                    ibmtts_send_to_audio(playback_queue_entry);
                    break;
                case IBMTTS_QET_INDEX_MARK:
                    /* Look up the index mark integer id in lookup table to
                       find string name and emit that name. */
                    markId = playback_queue_entry->data.markId;
                    mark_name = g_hash_table_lookup(ibmtts_index_mark_ht, &markId);
                    if (NULL == mark_name) {
                        DBG("Ibmtts: markId %d returned by IBM TTS not found in lookup table.",
                            markId)
                    } else {
                        DBG("Ibmtts: reporting index mark |%s|.", mark_name);
                        module_report_index_mark(mark_name);
                        DBG("Ibmtts: index mark reported.");
                        /* If pause requested, wait for an end-of-sentence index mark. */
                        if (ibmtts_pause_requested) {
                            if (0 == strncmp(mark_name, SD_MARK_BODY, SD_MARK_BODY_LEN)) {
                                DBG("Ibmtts: Pause requested in playback thread.  Stopping.");
                                ibmtts_stop_play_requested = IBMTTS_TRUE;
                            }
                        }
                    }
                    break;
                case IBMTTS_QET_SOUND_ICON:
                    ibmtts_play_file(playback_queue_entry->data.sound_icon_filename);
                    break;
                case IBMTTS_QET_BEGIN:
                    module_report_event_begin();
                    break;
                case IBMTTS_QET_END:
                    module_report_event_end();
                    break;
            }

            ibmtts_delete_playback_queue_entry(playback_queue_entry);
            playback_queue_entry = NULL;

            pthread_mutex_lock(&playback_queue_mutex);
            if (NULL != playback_queue) {
                playback_queue_entry = playback_queue->data;
                playback_queue = g_slist_remove(playback_queue, playback_queue->data);
            }
            pthread_mutex_unlock(&playback_queue_mutex);
        }
        if (ibmtts_stop_play_requested) DBG("Ibmtts: Stop or pause in playback thread.");
    }

    DBG("Ibmtts: Playback thread ended.......\n");

    pthread_exit(NULL);
}

/* Replaces all occurrences of "from" with "to" in msg.
   Returns count of replacements. */
static int
ibmtts_replace(char *from, char *to, GString *msg)
{
    int count = 0;
    int pos;
    int from_len = strlen(from);
    int to_len = strlen(to);
    char *p = msg->str;
    while (NULL != (p = strstr(p, from))) {
        pos = p - msg->str;
        g_string_erase(msg, pos, from_len);
        g_string_insert(msg, pos, to);
        p = msg->str + pos + to_len;
        ++count;
    }
    return count;
}

static void
ibmtts_subst_keys_cb(gpointer data, gpointer user_data)
{
    TIbmttsKeySubstitution *key_subst = data;
    GString *msg = user_data;
    ibmtts_replace(key_subst->key, key_subst->newkey, msg);
}

/* Given a Speech Dispatcher !KEY key sequence, replaces unspeakable
   or incorrectly spoken keys or characters with speakable ones.
   The subsitutions come from the KEY NAME SUBSTITUTIONS section of the
   config file.
   Caller is responsible for freeing returned string. */
static char *
ibmtts_subst_keys(char *key)
{
    GString *tmp = g_string_sized_new(30);
    g_string_append(tmp, key);

    GList *keyTable = g_hash_table_lookup(IbmttsKeySubstitution, msg_settings.language);

    if (keyTable)
        g_list_foreach(keyTable, ibmtts_subst_keys_cb, tmp);

    /* Hyphen hangs IBM TTS */
    if (0 == strcmp(tmp->str, "-"))
        g_string_assign(tmp, "hyphen");

    return g_string_free(tmp, FALSE);
}

/* Given a sound icon name, searches for a file to play and if found
   returns the filename.  Returns NULL if none found.  Caller is responsible
   for freeing the returned string. */
/* TODO: These current assumptions should be dealt with:
        Sound icon files are in a single directory (IbmttsSoundIconFolder).
        The name of each icon is symlinked to a .wav file.
   If you have installed the free(b)soft sound-icons package under
   Debian, then these assumptions are true, but what about other distros
   and OSes? */
static char *
ibmtts_search_for_sound_icon(const char *icon_name)
{
    char *fn = NULL;
#if HAVE_SNDFILE
    if (0 == strlen(IbmttsSoundIconFolder)) return fn;
    GString *filename = g_string_new(IbmttsSoundIconFolder);
    filename = g_string_append(filename, icon_name);
    if (g_file_test(filename->str, G_FILE_TEST_EXISTS))
        fn = filename->str;
/*
    else {
        filename = g_string_assign(filename, g_utf8_strdown(filename->str, -1));
        if (g_file_test(filename->str, G_FILE_TEST_EXISTS))
            fn = filename->str;
    }
*/
    g_string_free(filename, FALSE);
#endif
    return fn;
}

/* Plays the specified audio file. */
static TIbmttsBool
ibmtts_play_file(char *filename)
{
    TIbmttsBool result = IBMTTS_TRUE;
#if HAVE_SNDFILE
    int ret;
    int subformat;
    sf_count_t items;
    sf_count_t readcount;
    SNDFILE* sf;
    SF_INFO sfinfo;

    DBG("Ibmtts: Playing |%s|", filename);
    memset (&sfinfo, 0, sizeof (sfinfo));
    sf = sf_open(filename, SFM_READ, &sfinfo);
    subformat = sfinfo.format & SF_FORMAT_SUBMASK ;
    items = sfinfo.channels * sfinfo.frames;
    DBG("Ibmtts: frames = %ld, channels = %d", (long) sfinfo.frames, sfinfo.channels);
    DBG("Ibmtts: samplerate = %i, items = %ld", sfinfo.samplerate, items);
    DBG("Ibmtts: major format = 0x%08X, subformat = 0x%08X, endian = 0x%08X",
        sfinfo.format & SF_FORMAT_TYPEMASK, subformat, sfinfo.format & SF_FORMAT_ENDMASK);
    if (sfinfo.channels < 1 || sfinfo.channels > 2) {
        DBG("Ibmtts: ERROR: channels = %d.\n", sfinfo.channels);
        result = IBMTTS_FALSE;
        goto cleanup1;
    }
    if (sfinfo.frames > 0x7FFFFFFF) {
        DBG("Ibmtts: ERROR: Unknown number of frames.");
        result = IBMTTS_FALSE;
        goto cleanup1;
    }
    if (subformat == SF_FORMAT_FLOAT || subformat == SF_FORMAT_DOUBLE) {
        /* Set scaling for float to integer conversion. */
        sf_command (sf, SFC_SET_SCALE_FLOAT_INT_READ, NULL, SF_TRUE);
    }
    AudioTrack track;
    track.num_samples = sfinfo.frames;
    track.num_channels = sfinfo.channels;
    track.sample_rate = sfinfo.samplerate;
    track.bits = 16;
    track.samples = xmalloc(items * sizeof(short));
    if (NULL == track.samples) {
        DBG("Ibmtts: ERROR: Cannot allocate audio buffer.");
        result = IBMTTS_FALSE;
        goto cleanup1;
    }
    readcount = sf_read_short(sf, (short *) track.samples, items);
    DBG("Ibmtts: read %ld items from audio file.", readcount);

    if (readcount > 0) {
        track.num_samples = readcount / sfinfo.channels;
        DBG("Ibmtts: Sending %i samples to audio.", track.num_samples);
        /* Volume is controlled by the synthesizer.  Always play at normal on audio device. */
        spd_audio_set_volume(ibmtts_audio_id, 0);
        int ret = spd_audio_play(ibmtts_audio_id, track);
        if (ret < 0) {
            DBG("ERROR: Can't play track for unknown reason.");
            result = IBMTTS_FALSE;
            goto cleanup2;
        }
        DBG("Ibmtts: Sent to audio.");
    }
cleanup2:
    xfree(track.samples);
cleanup1:
    sf_close(sf);
#endif
    return result;
}

#include "module_main.c"