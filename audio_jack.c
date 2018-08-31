/*
 * jack output driver. This file is part of Shairport Sync.
 * Copyright (c) 2018 Mike Brady <mikebrady@iercom.net>
 *
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "audio.h"
#include "common.h"
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/transport.h>

enum ift_type {
  IFT_frame_left_sample = 0,
  IFT_frame_right_sample,
} ift_type;

// Four seconds buffer -- should be plenty
#define buffer_size 44100 * 4 * 2 * 2

static pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

char *audio_lmb, *audio_umb, *audio_toq, *audio_eoq;
size_t audio_occupancy; // this is in frames, not bytes. A frame is a left and
                        // right sample, each 16 bits, hence 4 bytes

// static void help(void);
int init(int, char **);
// static void onmove_cb(void *, int);
// static void deinit(void);
void jack_start(int, int);
int play(void *, int);
void jack_stop(void);
int jack_is_running(void);
// static void onmove_cb(void *, int);
int jack_delay(long *);
void jack_flush(void);

audio_output audio_jack = {.name = "jack",
                           .help = NULL,
                           .init = &init,
                           .deinit = NULL,
                           .start = &jack_start,
                           .stop = &jack_stop,
                           .is_running = &jack_is_running,
                           .flush = &jack_flush,
                           .delay = &jack_delay,
                           .play = &play,
                           .volume = NULL,
                           .parameters = NULL,
                           .mute = NULL};

typedef jack_default_audio_sample_t sample_t;

const double PI = 3.14;

jack_port_t *left_port;
jack_port_t *right_port;
long offset = 0;
int transport_aware = 0;
jack_transport_state_t transport_state;

int client_is_open;
jack_client_t *client;
jack_nframes_t sample_rate;

jack_latency_range_t latest_latency_range;
int64_t time_of_latest_latency_range;

int play(void *buf, int samples) {
  // debug(1,"jack_play of %d samples.",samples);
  // copy the samples into the queue
  size_t bytes_to_transfer = samples * 2 * 2;
  size_t space_to_end_of_buffer = audio_umb - audio_eoq;
  if (space_to_end_of_buffer >= bytes_to_transfer) {
    memcpy(audio_eoq, buf, bytes_to_transfer);
    pthread_mutex_lock(&buffer_mutex);
    audio_occupancy += samples;
    audio_eoq += bytes_to_transfer;
    pthread_mutex_unlock(&buffer_mutex);
  } else {
    memcpy(audio_eoq, buf, space_to_end_of_buffer);
    buf += space_to_end_of_buffer;
    memcpy(audio_lmb, buf, bytes_to_transfer - space_to_end_of_buffer);
    pthread_mutex_lock(&buffer_mutex);
    audio_occupancy += samples;
    audio_eoq = audio_lmb + bytes_to_transfer - space_to_end_of_buffer;
    pthread_mutex_unlock(&buffer_mutex);
  }

  if ((audio_occupancy >= 11025 * 2 * 2)) {
  }

  return 0;
}

void deinterleave_and_convert_stream(const char *interleaved_frames,
                                     const sample_t *jack_frame_buffer,
                                     jack_nframes_t number_of_frames, enum ift_type side) {
  jack_nframes_t i;
  short *ifp = (short *)interleaved_frames;
  sample_t *fp = (sample_t *)jack_frame_buffer;
  if (side == IFT_frame_right_sample)
    ifp++;
  for (i = 0; i < number_of_frames; i++) {
    short sample = *ifp;
    sample_t converted_value;
    if (sample >= 0)
      converted_value = (1.0 * sample) / SHRT_MAX;
    else
      converted_value = -(1.0 * sample) / SHRT_MIN;
    *fp = converted_value;
    ifp++;
    ifp++;
    fp++;
  }
}

int jack_stream_write_cb(jack_nframes_t nframes, __attribute__((unused)) void *arg) {

  sample_t *left_buffer = (sample_t *)jack_port_get_buffer(left_port, nframes);
  sample_t *right_buffer = (sample_t *)jack_port_get_buffer(right_port, nframes);

  size_t frames_we_can_transfer = nframes;
  // lock
  pthread_mutex_lock(&buffer_mutex);
  if (audio_occupancy < frames_we_can_transfer) {
    frames_we_can_transfer = audio_occupancy;
  }

  // frames we can transfer will never be greater than the frames available

  if (frames_we_can_transfer * 2 * 2 <= (size_t)(audio_umb - audio_toq)) {
    // the bytes are all in a row in the audio buffer
    deinterleave_and_convert_stream(audio_toq, &left_buffer[0], frames_we_can_transfer,
                                    IFT_frame_left_sample);
    deinterleave_and_convert_stream(audio_toq, &right_buffer[0], frames_we_can_transfer,
                                    IFT_frame_right_sample);
    audio_toq += frames_we_can_transfer * 2 * 2;
  } else {
    // the bytes are in two places in the audio buffer
    size_t first_portion_to_write = (audio_umb - audio_toq) / (2 * 2);
    if (first_portion_to_write != 0) {
      deinterleave_and_convert_stream(audio_toq, &left_buffer[0], first_portion_to_write,
                                      IFT_frame_left_sample);
      deinterleave_and_convert_stream(audio_toq, &right_buffer[0], first_portion_to_write,
                                      IFT_frame_right_sample);
    }
    deinterleave_and_convert_stream(audio_lmb, &left_buffer[first_portion_to_write],
                                    frames_we_can_transfer - first_portion_to_write,
                                    IFT_frame_left_sample);
    deinterleave_and_convert_stream(audio_lmb, &right_buffer[first_portion_to_write],
                                    frames_we_can_transfer - first_portion_to_write,
                                    IFT_frame_right_sample);
    audio_toq = audio_lmb + (frames_we_can_transfer - first_portion_to_write) * 2 * 2;
  }
  // debug(1,"transferring %u frames",frames_we_can_transfer);
  audio_occupancy -= frames_we_can_transfer;
  pthread_mutex_unlock(&buffer_mutex);
  // unlock
  
 // now, if there are any more frames to put into the buffer, fill them with
  // silence
  jack_nframes_t i;

  for (i = frames_we_can_transfer; i < nframes; i++) {
    left_buffer[i] = 0.0;
    right_buffer[i] = 0.0;
  }
  
  jack_port_get_latency_range(left_port, JackPlaybackLatency, &latest_latency_range);
  time_of_latest_latency_range = get_absolute_time_in_fp();
 
  return 0;
}

void default_jack_error_callback(const char *desc) {
  debug(2,"jackd error: \"%s\"",desc);
}

void default_jack_info_callback(const char *desc) {
  inform("jackd information: \"%s\"",desc);
}

int jack_is_running() {
  int reply = -1; 
  // if the client is open and initialised, see if the status is "rolling"
  if (client_is_open) {
    jack_position_t pos;
    jack_transport_state_t 	transport_state = jack_transport_query (client, &pos);
    if (transport_state == JackTransportRolling)
      reply = 0;
    else
      reply = -2;
  }
  return reply;
}

int init(__attribute__((unused)) int argc, __attribute__((unused)) char **argv) {
  config.audio_backend_latency_offset = 0;
  config.audio_backend_buffer_desired_length = 0.15;

  // get settings from settings file first, allow them to be overridden by
  // command line options

  // do the "general" audio  options. Note, these options are in the "general" stanza!
  parse_general_audio_options();
  
  // other options would be picked up here...

  jack_set_error_function(default_jack_error_callback); 	
  jack_set_info_function(default_jack_info_callback);
  client_is_open = 0;

  // allocate space for the audio buffer
  audio_lmb = malloc(buffer_size);
  if (audio_lmb == NULL)
    die("Can't allocate %d bytes for jackaudio buffer.", buffer_size);
  audio_toq = audio_eoq = audio_lmb;
  audio_umb = audio_lmb + buffer_size;
  audio_occupancy = 0; // frames
  return 0;
}

void jack_start(__attribute__((unused)) int i_sample_rate, __attribute__((unused)) int i_sample_format) {
  debug(1,"jack start");
  // int reply = -1;
  
  // see if the client is running. If not, try to open and initialise it 
  if (client_is_open == 0) {
    jack_status_t status;
    client = jack_client_open("Shairport Sync", JackNoStartServer, &status);
    if (client) {
      jack_set_process_callback(client, jack_stream_write_cb, 0);
      left_port = jack_port_register(client, "Left", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
      right_port = jack_port_register(client, "Right", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
      sample_rate = jack_get_sample_rate(client);
      debug(1, "jackaudio sample rate = %" PRId32 ".", sample_rate);
      if (jack_activate(client)) {
        debug(1, "jackaudio cannot activate client");
      } else { 
        client_is_open = 1;
        debug(1, "jackaudio client opened.");
      } 
    }   
  }
  
  if (client_is_open == 0)
    debug(1,"cannot open a jack client for a play session");
}

int jack_delay(long *the_delay) {
  int64_t time_now = get_absolute_time_in_fp();
  int64_t delta = time_now - time_of_latest_latency_range;
  
  int64_t frames_processed_since_latest_latency_check = (delta * 44100) >> 32;
  
  // debug(1,"delta: %" PRId64 " frames.",frames_processed_since_latest_latency_check);
  
  *the_delay = latest_latency_range.min + audio_occupancy - frames_processed_since_latest_latency_check;

  // debug(1,"reporting a delay of %d frames",*the_delay);

  return 0;
}

void jack_flush() {
    debug(1,"jack flush");
  // lock
  pthread_mutex_lock(&buffer_mutex);
  audio_toq = audio_eoq = audio_lmb;
  audio_umb = audio_lmb + buffer_size;
  audio_occupancy = 0; // frames
  pthread_mutex_unlock(&buffer_mutex);
  // unlock
}

void jack_stop(void) {
  debug(1,"jack stop");
}