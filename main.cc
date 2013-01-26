#include <ncurses.h>

#include <vector>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <alsa/asoundlib.h>
#include <math.h>

#include "dywapitchtrack.h"

const int kThreshold = 20;

enum {
  COL_NEUTRAL,
  COL_OK,
  COL_WARN,
  COL_NOTE,
};

enum KeyDisplay {
  DISPLAY_FLAT,
  DISPLAY_SHARP,
};
static KeyDisplay s_key_display = DISPLAY_SHARP;
const char *note_name[2][12] = {
  { "A", "Bb", "B", "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab" },
  { "A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#" },
};

class StatCounter {
public:
  struct Counter {
    int flat;
    int ok;
    int sharp;
  };
  StatCounter(int max_note) : note_count_(max_note),
                              counters_(new Counter [max_note]) {
    Reset();
  }
  ~StatCounter() { delete [] counters_; }

  void Reset() {
    memset(counters_, 0, note_count_ * sizeof(Counter));
  }
  void CountOk(int note) {
    if (note < 0 || note >= note_count_) return;
    ++counters_[note].ok;
  }
  void CountFlat(int note) {
    if (note < 0 || note >= note_count_) return;
    ++counters_[note].flat;
  }
  void CountSharp(int note) {
    if (note < 0 || note >= note_count_) return;
    ++counters_[note].sharp;
  }
  const int size() const { return note_count_; }
  const Counter* get_stat_for(int note) const {
    if (note < 0 || note >= note_count_) return NULL;
    return &counters_[note];
  }

private:
  const int note_count_;
  Counter *const counters_;
};
static StatCounter sStatCounter(34);

bool kShowCount = false;   // useful for debugging.

void show_menu(WINDOW *display, int row) {
  mvwprintw(display, row++, 2,  "---- Shortcuts ----");
  mvwprintw(display, row++, 2, "<space>: reset stats.");
  if (s_key_display == DISPLAY_FLAT) {
    mvwprintw(display, row++, 2, "# or s : show in sharp.");
  } else {
    mvwprintw(display, row++, 2, "b      : show in flat.");
  }
  mvwprintw(display, row++, 2,   "c      : as %s",
            kShowCount ? "percent" : "raw count");
}

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

void print_stats(WINDOW *display, WINDOW *flat, WINDOW *sharp) {
  int kStartX = 30;
  int kStartY = 3;
  int kStringSpace = 16;
  int kHalftoneSpace = 4;
  wbkgd(display, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
  werase(flat); wrefresh(flat);
  werase(sharp); wrefresh(sharp);
  werase(display);
  print_strings(display, kStartX, kStartY, kStringSpace, kHalftoneSpace);
  // Let's first see how many counts we have, so that we can discard notes
  // that are contributing less than 5% or so
  std::vector<int> percentile_counter;
  for (int note = 0; note < sStatCounter.size(); ++note) {
    const StatCounter::Counter *counter = sStatCounter.get_stat_for(note);
    if (!counter) continue;
    const int count = counter->flat + counter->ok + counter->sharp;
    if (!count) continue;
    percentile_counter.push_back(count);
  }
  std::sort(percentile_counter.begin(), percentile_counter.end());
  int require_min_count = 10;
  if (!percentile_counter.empty()) {
    require_min_count =
      std::max(require_min_count,
               percentile_counter[percentile_counter.size() / 10]);
    mvwprintw(display, 4, 1, "min-count: %d", require_min_count);
  }
  int total_scored = 0, total_in_tune = 0;
  for (int note = 0; note < sStatCounter.size(); ++note) {
    const StatCounter::Counter *counter = sStatCounter.get_stat_for(note);
    if (!counter) continue;
    const int note_count = counter->flat + counter->ok + counter->sharp;
    if (note_count == 0)
      continue;
    if (note_count <= require_min_count && !kShowCount)
      continue;  // Don't show noise unless raw count is required.
    total_scored += note_count;
    total_in_tune += counter->ok;

    // Each string covers 7 half-tones in 1st pos.
    const int cello_string = note / 7;
    const int pitch_pos = note % 7;
    const int pitch_screen_pos_y = kStartY + kHalftoneSpace * pitch_pos;
    const int string_screen_pos_x = kStartX + kStringSpace * cello_string - 3;
    const int bargraph_width = 12;
    const char *display_note = note_name[s_key_display][(note + 3)% 12];
    if (counter->flat) {
      const int percent = 100 * counter->flat / note_count;
      mvwprintw(display, pitch_screen_pos_y - 1, string_screen_pos_x,
                " ^ %3d%s", kShowCount ? counter->flat : percent,
                kShowCount ? "" : "%");
      mvwchgat(display, pitch_screen_pos_y - 1, string_screen_pos_x + 3,
               bargraph_width / 100.0 * percent, 0, COL_WARN, NULL);
    }
    {
      const int percent = 100 * counter->ok / note_count;
      mvwprintw(display, pitch_screen_pos_y, string_screen_pos_x,
                "%2s %3d%s", display_note, kShowCount ? counter->ok : percent,
                kShowCount ? "" : "%");
      mvwchgat(display, pitch_screen_pos_y, string_screen_pos_x + 3,
               bargraph_width / 100.0 * percent, 0, COL_OK, NULL);
    }

    if (counter->sharp) {
      const int percent = 100 * counter->sharp / note_count;
      mvwprintw(display, pitch_screen_pos_y + 1, string_screen_pos_x,
                " v %3d%s", kShowCount ? counter->sharp : percent,
                kShowCount ? "" : "%");
      mvwchgat(display, pitch_screen_pos_y + 1, string_screen_pos_x + 3,
               bargraph_width / 100.0 * percent, 0, COL_WARN, NULL);
    }
  }
  if (total_scored > 0) {
    mvwprintw(display, 1, 1, "Statistic. Total %.1f%% in tune.",
              100.0 * total_in_tune / total_scored);
  }
  show_menu(display, LINES - 10);
  wrefresh(display);
}

void print_freq(double f, WINDOW *display, WINDOW *flat, WINDOW *sharp) {
  int kStartX = 30;
  int kStartY = 3;
  int kStringSpace = 16;
  int kHalftoneSpace = 4;
  wbkgd(display, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
  werase(display);
  werase(flat);
  werase(sharp);
  print_strings(display, kStartX, kStartY, kStringSpace, kHalftoneSpace);
  wrefresh(display);
  if (f < 64 || f > 880) {
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
    sStatCounter.CountFlat(scale_above_C);
    in_tune = false;
  }
  else if (cent > kThreshold) {
    wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
    wbkgd(sharp, COLOR_PAIR(COL_WARN));
    sStatCounter.CountSharp(scale_above_C);
    in_tune = false;
  }
  else {
    sStatCounter.CountOk(scale_above_C);
  }
  wrefresh(flat);
  wrefresh(sharp);
  // Each string covers 7 half-tones in 1st pos.
  const int cello_string = scale_above_C / 7;
  const int pitch_pos = scale_above_C % 7;
  mvwprintw(display, 0, 0, "%4.1fHz", f);
  wcolor_set(display, in_tune ? COL_OK : COL_WARN, NULL);
  const int pitch_screen_pos_y = kStartY + kHalftoneSpace * pitch_pos;
  const int string_screen_pos_x = kStartX + kStringSpace * cello_string - 6;

  mvwprintw(display, pitch_screen_pos_y, string_screen_pos_x,
            "      %-7s", note_name[s_key_display][note]);
  if (cent < - kThreshold) {
    mvwprintw(display, pitch_screen_pos_y - 1, string_screen_pos_x,
              "      |      ");
  }
  else if (cent > kThreshold) {
    mvwprintw(display, pitch_screen_pos_y + 1, string_screen_pos_x,
              "      |      ");
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

  const char* pcm_device = "default";

  if (argc > 2) {
    fprintf(stderr, "usage: %s <pcm-device>\n", argv[0]);
    return 1;
  }
  if (argc > 1) {
    pcm_device = argv[1];
  }

  if ((err = snd_pcm_open (&capture_handle, pcm_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
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

  const int kPitchDisplay = 3;
  WINDOW *flat_pitch = newwin(kPitchDisplay, COLS, 0, 0);
  WINDOW *sharp_pitch = newwin(kPitchDisplay, COLS,
                                      LINES - kPitchDisplay, 0);
  WINDOW *display = newwin(LINES - 2 * kPitchDisplay, COLS,
                           kPitchDisplay, 0);
  nodelay(display, true);

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
      print_stats(display, flat_pitch, sharp_pitch);
    } else {
      double freq = dywapitch_computepitch(&tracker, analyze_buf);
      print_freq(freq, display, flat_pitch, sharp_pitch);
    }
    switch (wgetch(display)) {
    case 'b': case 'B': s_key_display = DISPLAY_FLAT; break;
    case '#': case 's': case 'S': s_key_display = DISPLAY_SHARP; break;
    case ' ': sStatCounter.Reset(); break;
    case 'c': kShowCount = !kShowCount; break;
    }
  }
	
  snd_pcm_close(capture_handle);
  return 0;
}
