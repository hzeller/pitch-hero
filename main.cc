#include <ncurses.h>

#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <math.h>

#include "dywapitchtrack.h"

const int kThreshold = 25;

enum {
  COL_NEUTRAL,
  COL_OK,
  COL_WARN,
  COL_NOTE,
};

/*
C  G  D  A
C# G# D# A#
D  A  E  B
D# A# F  C
E  B  F# C#
F  C  G  D
F# C# G# D#
*/

const char* note_name[] = {
  "   A   ",
  "A# / Bb",
  "   B   ",
  "   C   ",
  "C# / Db",
  "   D   ",
  "D# / Eb",
  "   E   ",
  "   F   ",
  "F# / Gb",
  "   G   ",
  "G# / Ab",
  "   A   ",  // rounding can bring us here as well.
};

void print_strings(WINDOW *display,
                   int start_x, int start_y,
                   int string_space, int halftone_space) {
  const int kStrings = 4;
  for (int x = 0; x < (kStrings - 1) * string_space; ++x) {
    mvwprintw(display, start_y, start_x + x, "-");
  }
  for (int s = 0; s < kStrings; ++s) {
    for (int y = 0; y < 7 * halftone_space; ++y) {
      mvwprintw(display, start_y + y, start_x + string_space * s,
                y % halftone_space == 0 ? "+" : "|");
    }
  }
}

void print_freq(double f, WINDOW *display, WINDOW *flat, WINDOW *sharp) {
  int kStartX = 10;
  int kStartY = 3;
  int kStringSpace = 10;
  int kHalftoneSpace = 3;
  wbkgd(display, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
  werase(display);
  werase(flat);
  werase(sharp);
  print_strings(display, kStartX, kStartY, kStringSpace, kHalftoneSpace);
  wrefresh(display);
  if (f < 64 || f > 640) {
    wrefresh(display);
    wrefresh(flat);
    wrefresh(sharp);    
    return;
  }
  static const double base = 55.0;  // 440 / 2 / 2 = low A
  static const double d = exp(log(2) / 1200);
  const double cent_above_base = log(f / base) / log(d);
  const int scale_above_C = round(cent_above_base / 100.0) - 3;

  // Press into regular scale
  double scale = fmod(cent_above_base, 1200.0);
  scale /= 100.0;
  int rounded = round(scale);
  double cent = 100 * (scale - rounded);
  int note = rounded % 12;   // rounded can be 12.

  bool in_tune = true;
  if (cent < - kThreshold) {
    wbkgd(flat, COLOR_PAIR(COL_WARN));
    wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
    in_tune = false;
  }
  else if (cent > kThreshold) {
    wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
    wbkgd(sharp, COLOR_PAIR(COL_WARN));
    in_tune = false;
  }
  if (fabs(cent) > 5) {
    mvwprintw(cent < 0 ? flat : sharp, 2, COLS / 2, "%2.f", cent);
  }
  wrefresh(flat);
  wrefresh(sharp);
  // Each string covers 7 half-tones in 1st pos.
  const int cello_string = scale_above_C / 7;
  const int pitch_pos = scale_above_C % 7;
  mvwprintw(display, 0, 0, "%4.1f", f, cello_string);
  wcolor_set(display, in_tune ? COL_OK : COL_WARN, NULL);
  const int pitch_screen_pos_y = kStartY + kHalftoneSpace * pitch_pos;
  const int string_screen_pos_x = kStartX + kStringSpace * cello_string - 3;
  mvwprintw(display, pitch_screen_pos_y, string_screen_pos_x,
            note_name[note]);
  if (cent < - kThreshold) {
    mvwprintw(display, pitch_screen_pos_y - 1, string_screen_pos_x,
              "   |   ");
  }
  else if (cent > kThreshold) {
    mvwprintw(display, pitch_screen_pos_y + 1, string_screen_pos_x,
              "   |   ");
  }
  wrefresh(display);
}

static unsigned int kSampleRate = 44100;
static int kBlock = 2048;
int main (int argc, char *argv[]) {
  int i;
  int err;
  short buf[4096];
  snd_pcm_t *capture_handle = NULL;
  snd_pcm_hw_params_t *hw_params = NULL;

#if 0
  print_freq(55);
  print_freq(109);
  print_freq(110);
  print_freq(220);
  print_freq(440);
  print_freq(710);
  print_freq(622);
  return 0;
#endif

  if (argc != 2) {
    fprintf(stderr, "usage: %s <pcm-device>\n", argv[0]);
    return 1;
  }

  if ((err = snd_pcm_open (&capture_handle, argv[1], SND_PCM_STREAM_CAPTURE, 0)) < 0) {
    fprintf (stderr, "cannot open audio device %s (%s)\n", 
             argv[1],
             snd_strerror (err));
    return 1;
  }
		   
  if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
    fprintf (stderr, "cannot allocate hardware parameter structure (%s)\n",
             snd_strerror (err));
    return 1;
  }
				 
  if ((err = snd_pcm_hw_params_any (capture_handle, hw_params)) < 0) {
    fprintf (stderr, "cannot initialize hardware parameter structure (%s)\n",
             snd_strerror (err));
    return 1;
  }
	
  if ((err = snd_pcm_hw_params_set_access (capture_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    fprintf (stderr, "cannot set access type (%s)\n",
             snd_strerror (err));
    return 1;
  }
	
  if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
    fprintf (stderr, "cannot set sample format (%s)\n",
             snd_strerror (err));
    return 1;
  }
	
  if ((err = snd_pcm_hw_params_set_rate_near(capture_handle,
                                             hw_params, &kSampleRate, 0)) < 0) {
    fprintf (stderr, "cannot set sample rate (%s)\n",
             snd_strerror (err));
    return 1;
  }
	
  if ((err = snd_pcm_hw_params_set_channels (capture_handle, hw_params, 1)) < 0) {
    fprintf (stderr, "cannot set channel count (%s)\n",
             snd_strerror (err));
    return 1;
  }
	
  if ((err = snd_pcm_hw_params (capture_handle, hw_params)) < 0) {
    fprintf (stderr, "cannot set parameters (%s)\n",
             snd_strerror (err));
    return 1;
  }
	
  snd_pcm_hw_params_free (hw_params);
	
  if ((err = snd_pcm_prepare (capture_handle)) < 0) {
    fprintf (stderr, "cannot prepare audio interface for use (%s)\n",
             snd_strerror (err));
    return 1;
  }

  initscr();
  start_color();
  curs_set(0);

  init_pair(COL_NEUTRAL, COLOR_WHITE, COLOR_BLACK);
  init_pair(COL_OK, COLOR_BLACK, COLOR_GREEN);
  init_pair(COL_WARN, COLOR_BLACK, COLOR_RED);
  init_pair(COL_NOTE, COLOR_BLACK, COLOR_YELLOW);

  const int kPitchDisplay = 4;
  WINDOW *flat_pitch = newwin(kPitchDisplay, COLS, 0, 0);
  WINDOW *sharp_pitch = newwin(kPitchDisplay, COLS,
                                      LINES - kPitchDisplay, 0);
  WINDOW *display = newwin(LINES - 2 * kPitchDisplay, COLS,
                           kPitchDisplay, 0);

  const int sample_count = 2 * dywapitch_neededsamplecount(60);
  fprintf(stderr, "Using %d samples.\n", sample_count);
  dywapitchtracker tracker;
  dywapitch_inittracking(&tracker, sample_count);
  int small_sample = sample_count / 16;
  short *read_buf = new short [ small_sample ];
  double *analyze_buf = new double [ sample_count ];
  for (;;) {
    if ((err = snd_pcm_readi (capture_handle, read_buf,
                              small_sample)) != small_sample) {
      fprintf (stderr, "read from audio interface failed (%s)\n",
               snd_strerror (err));
      return 1;
    }
    const int tail_buffer = sample_count - small_sample;
    memmove(analyze_buf, analyze_buf + small_sample,
            sizeof(double) * (tail_buffer));
    int max_val = 0;
    for (int i = 0; i < small_sample; ++i) {
      if (abs(read_buf[i]) > max_val)
        max_val = abs(read_buf[i]);
      analyze_buf[tail_buffer + i] = read_buf[i] / 32768.0;
    }
    if (max_val < 2000) {
      print_freq(0, display, flat_pitch, sharp_pitch);
    } else {
      double freq = dywapitch_computepitch(&tracker, analyze_buf);
      print_freq(freq, display, flat_pitch, sharp_pitch);
    }
  }
	
  snd_pcm_close(capture_handle);
  return 0;
}
