// First hack. Get rid of global variables and stuff.
#include <ncurses.h>

#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <algorithm>
#include <vector>


#include "dywapitchtrack.h"

static const int kMaxNotesAboveC = 35;

int cent_threshold = 20;
bool paused = false;

enum {
  COL_NEUTRAL,
  COL_OK,
  COL_WARN,
  COL_SELECT,
  COL_HEADLINE,
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

class StringBoard {
public:
  StringBoard(WINDOW *display, int x, int y)
    : kStrings(4), kStringSpace(16), kHalftoneSpace(4), // constants for now.
      display_(display), origin_x_(x), origin_y_(y) {
  }
  
  void PrintStringBoard() {
    for (int x = 0; x < (kStrings - 1) * kStringSpace; ++x) {
      mvwprintw(display_, origin_y_, origin_x_ + x, "-");
    }
    for (int s = 0; s < kStrings; ++s) {
      for (int y = 0; y < 7 * kHalftoneSpace; ++y) {
        mvwprintw(display_, origin_y_ + y, origin_x_ + kStringSpace * s,
                  y % kHalftoneSpace == 0 ? "+" : "|");
      }
    }
  }

  void PrintNote(const char *name, int str, int position, float cent,
                 bool in_tune) {
    wcolor_set(display_, in_tune ? COL_OK : COL_WARN, NULL);
    const int pitch_screen_pos_y = origin_y_ + kHalftoneSpace * position;
    const int string_screen_pos_x = origin_x_ + kStringSpace * str - 6;
    
    mvwprintw(display_, pitch_screen_pos_y, string_screen_pos_x,
              "      %-7s", name);
    const int bargraph_width = 13;
    if (cent < -5) {
      int bar_len = bargraph_width / 50.0 * -cent;
      mvwchgat(display_, pitch_screen_pos_y - 1,
               string_screen_pos_x + bargraph_width - bar_len,
               bar_len, 0, COL_WARN, NULL);
    }
    else if (cent > 5) {
      mvwchgat(display_, pitch_screen_pos_y + 1, string_screen_pos_x,
               bargraph_width / 50.0 * cent, 0, COL_WARN, NULL);
    }
  }

  void PrintBargraph(const char *note_name, int str, int position,
                     int show_count,
                     int count_flat, int count_in_tune, int count_sharp) {
    const int sum = count_flat + count_in_tune + count_sharp;
    if (sum == 0)
      return;
    const int pitch_screen_pos_y = origin_y_ + kHalftoneSpace * position;
    const int string_screen_pos_x = origin_x_ + kStringSpace * str - 3;
    const int bargraph_width = 12;
    if (count_flat) {
      const int percent = 100 * count_flat / sum;
      mvwprintw(display_, pitch_screen_pos_y - 1, string_screen_pos_x,
                " ^ %3d%s", show_count ? count_flat : percent,
                show_count ? "" : "%");
      mvwchgat(display_, pitch_screen_pos_y - 1, string_screen_pos_x + 3,
               bargraph_width / 100.0 * percent, 0, COL_WARN, NULL);
    }
    {
      const int percent = 100 * count_in_tune / sum;
      mvwprintw(display_, pitch_screen_pos_y, string_screen_pos_x,
                "%2s %3d%s", note_name, show_count ? count_in_tune : percent,
                show_count ? "" : "%");
      mvwchgat(display_, pitch_screen_pos_y, string_screen_pos_x + 3,
               bargraph_width / 100.0 * percent, 0, COL_OK, NULL);
    } 
    if (count_sharp) {
      const int percent = 100 * count_sharp / sum;
      mvwprintw(display_, pitch_screen_pos_y + 1, string_screen_pos_x,
                " v %3d%s", show_count ? count_sharp : percent,
                show_count ? "" : "%");
      mvwchgat(display_, pitch_screen_pos_y + 1, string_screen_pos_x + 3,
               bargraph_width / 100.0 * percent, 0, COL_WARN, NULL);
    }
  }

private:
  const int kStrings;
  const int kStringSpace;
  const int kHalftoneSpace;
  
  WINDOW *const display_;
  const int origin_x_;
  const int origin_y_;
};

class StatCounter {
public:
  struct Counter {
    Counter() : flat(0), ok(0), sharp(0) {}
    int flat;
    int ok;
    int sharp;
  };
  StatCounter(int max_note) : note_count_(max_note),
                              histogram_(new Histogram [max_note]) {
    Reset();
  }
  ~StatCounter() { delete [] histogram_; }

  void Reset() {
    memset(histogram_, 0, note_count_ * sizeof(Histogram));
  }
  void Count(int note, int cent) {
    if (note < 0 || note >= note_count_) return;
    int index = (cent + 50) / 5;
    if (index > 19) index = 19;
    ++histogram_[note].histogram[index];
  }

  const int size() const { return note_count_; }
  Counter get_stat_for(int note, int threshold) const {
    Counter result;
    if (note < 0 || note >= note_count_) return result;
    const Histogram &h = histogram_[note];
    for (int i = 0; i < 10 - threshold / 5; ++i) {
      result.flat += h.histogram[i];
    }
    for (int i = 10 - threshold / 5; i < 10 + threshold / 5; ++i) {
      result.ok += h.histogram[i];
    }
    for (int i = 10 + threshold / 5; i < 20; ++i) {
      result.sharp += h.histogram[i];
    }
    return result;
  }

private:
  struct Histogram {
    int histogram[20];  // 0..9 (flat -49..0) 10..19 (sharp 0..49)
  };
  const int note_count_;
  Histogram *const histogram_;
};
static StatCounter sStatCounter(kMaxNotesAboveC);

bool kShowCount = false;   // useful for debugging.

static double GetTime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

static void show_menu(WINDOW *display, int row) {
  int x = 0;
  wcolor_set(display, COL_HEADLINE, NULL);
  mvwprintw(display, row++, x,  " Shortcuts ");
  wcolor_set(display, COL_NEUTRAL, NULL);
  mvwprintw(display, row++, x, " <space>: reset stats.");
  if (s_key_display == DISPLAY_FLAT) {
    mvwprintw(display, row++, x, " # or s : show in sharp.");
  } else {
    mvwprintw(display, row++, x, " b      : show in flat.");
  }
  mvwprintw(display, row++, x,   " UP/DN  : threshold cent=%d",
            cent_threshold);

  wcolor_set(display, paused ? COL_SELECT : COL_NEUTRAL, NULL);
  mvwprintw(display, row++, x,   " p      : %spause listen   ",
            paused ? "un-" : "");
  if (paused) {  // let's sneak in a little blinking pause symbol :)
    wattron(display, A_BLINK);
    mvwprintw(display, row - 1, x + 4,   "||");
    wattroff(display, A_BLINK);
  }
  wcolor_set(display, kShowCount ? COL_SELECT : COL_NEUTRAL, NULL);
  mvwprintw(display, row++, x,   " c      : show %s",
            kShowCount ? "percent      " : "raw count");
  wcolor_set(display, COL_NEUTRAL, NULL);
  mvwprintw(display, row++, x, " q      : quit.");
}

static void print_percent_per_cutoff(WINDOW *display, int x, int y,
                                     int min_count, int bargraph_width) {
  wcolor_set(display, COL_HEADLINE, NULL);
  mvwprintw(display, y++, x, " Percentage in tune for      ");
  mvwprintw(display, y++, x, " given acceptance threshold. ");
  wcolor_set(display, COL_NEUTRAL, NULL);
  x += 1;
  mvwprintw(display, y++, x, "Cent %%-in-tune");
  for (int threshold = 5; threshold <= 45; threshold += 5) {
    int total_scored = 0;
    int total_in_tune = 0;
    for (int note = 0; note < sStatCounter.size(); ++note) {
      StatCounter::Counter counter = sStatCounter.get_stat_for(note, threshold);
      const int note_count = counter.flat + counter.ok + counter.sharp;
      if (note_count == 0 || note_count <= min_count)
        continue;
      total_scored += note_count;
      total_in_tune += counter.ok;
    }
    if (total_scored == 0)
      continue;
    const bool is_selected = threshold == cent_threshold;
    wcolor_set(display, is_selected ? COL_SELECT : COL_NEUTRAL, NULL);
    const float fraction = 1.0 * total_in_tune / total_scored;
    mvwprintw(display, y, x, "%s%3d %3.f%%%*s", is_selected ? ">" : " ",
              threshold, 100.0 * fraction, bargraph_width - 4, "");
    mvwchgat(display, y, x + 5, bargraph_width * fraction, 0, COL_OK, NULL);
    ++y;
  }
  wcolor_set(display, COL_NEUTRAL, NULL);
}

static void print_stats(WINDOW *display, WINDOW *flat, WINDOW *sharp) {
  int kStartX = 33;
  int kStartY = 3;
  wbkgd(display, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
  werase(flat); wrefresh(flat);
  werase(sharp); wrefresh(sharp);
  // Let's first see how many counts we have, so that we can discard notes
  // that are contributing less than 5% or so
  std::vector<int> percentile_counter;
  for (int note = 0; note < sStatCounter.size(); ++note) {
    StatCounter::Counter counter = sStatCounter.get_stat_for(note,
                                                             cent_threshold);
    const int count = counter.flat + counter.ok + counter.sharp;
    if (!count) continue;
    percentile_counter.push_back(count);
  }
  std::sort(percentile_counter.begin(), percentile_counter.end());
  int require_min_count = 10;
  if (!percentile_counter.empty()) {
    require_min_count =
      std::max(require_min_count,
               percentile_counter[percentile_counter.size() / 10]);
  }
  werase(display);
  int total_scored = 0, total_in_tune = 0;
  StringBoard board(display, kStartX, kStartY);
  board.PrintStringBoard();
  print_percent_per_cutoff(display, 0, 0, require_min_count, 19);

  for (int note = 0; note < sStatCounter.size(); ++note) {
    StatCounter::Counter counter = sStatCounter.get_stat_for(note,
                                                             cent_threshold);
    const int note_count = counter.flat + counter.ok + counter.sharp;
    if (note_count == 0)
      continue;
    if (note_count <= require_min_count && !kShowCount)
      continue;  // Don't show noise unless raw count is required.
    total_scored += note_count;
    total_in_tune += counter.ok;

    const int cello_string = note / 7;
    const int pitch_pos = note % 7;
    board.PrintBargraph(note_name[s_key_display][(note + 3)% 12],
                        cello_string, pitch_pos, kShowCount,
                        counter.flat, counter.ok, counter.sharp);
  }

  show_menu(display, LINES - 13);
  wrefresh(display);
}

static void print_freq(double f, int max_value,
                       WINDOW *display, WINDOW *flat, WINDOW *sharp) {
  int kStartX = 33;
  int kStartY = 3;
  wbkgd(display, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
  wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
  werase(display);
  werase(flat);
  werase(sharp);

  StringBoard board(display, kStartX, kStartY);
  board.PrintStringBoard();

  if (max_value > 0) {
    const float vu_db = 20 * (log(max_value / 32768.0) / log(10));
    // everything above -20 db we show
    const float kMinDB = -20;
    const int kVUWidth = 16;
    if (vu_db > kMinDB) {
      int vu_bar = kVUWidth * (vu_db - kMinDB) / -kMinDB;
      mvwchgat(display, 0, 1, vu_bar, 0, COL_OK, NULL);
    }
  }

  if (f < 64 || f > 650) {
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
  if (cent < - cent_threshold) {
    wbkgd(flat, COLOR_PAIR(COL_WARN));
    wbkgd(sharp, COLOR_PAIR(COL_NEUTRAL));
    in_tune = false;
  }
  else if (cent > cent_threshold) {
    wbkgd(flat, COLOR_PAIR(COL_NEUTRAL));
    wbkgd(sharp, COLOR_PAIR(COL_WARN));
    in_tune = false;
  }
  sStatCounter.Count(scale_above_C, cent);
  wrefresh(flat);
  wrefresh(sharp);

  //mvwprintw(display, 1, 0, "%4.1fHz", f);

  // Each string covers 7 half-tones in 1st pos.
  const int cello_string = scale_above_C / 7;
  const int pitch_pos = scale_above_C % 7;
  board.PrintNote(note_name[s_key_display][note], cello_string, pitch_pos,
                  in_tune, cent);
  wrefresh(display);
}

static unsigned int kSampleRate = 44100;
int main (int argc, char *argv[]) {
  int err;
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
  init_pair(COL_SELECT, COLOR_WHITE, COLOR_BLUE);
  init_pair(COL_HEADLINE, COLOR_BLACK, COLOR_WHITE);

  const int kPitchDisplay = 3;
  WINDOW *flat_pitch = newwin(kPitchDisplay, COLS, 0, 0);
  WINDOW *sharp_pitch = newwin(kPitchDisplay, COLS,
                                      LINES - kPitchDisplay, 0);
  WINDOW *display = newwin(LINES - 2 * kPitchDisplay, COLS,
                           kPitchDisplay, 0);
  nodelay(display, true);   // don't block for keypresses
  keypad(display, TRUE);   // make complex keys such as cursor work.

  const int sample_count = 2 * dywapitch_neededsamplecount(60);
  fprintf(stderr, "Using %d samples.\n", sample_count);
  dywapitchtracker tracker;
  dywapitch_inittracking(&tracker, sample_count);
  int small_sample = sample_count / 16;
  short *read_buf = new short [ small_sample ];
  double *analyze_buf = new double [ sample_count ];
  bool any_change = true;
  double last_keypress_time = -1;
  bool do_exit = false;
  while (!do_exit) {
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

    // Now, let's first check for keypresses that happened in the meantime.
    // The do create some keyboard noise, so if we detect one, then we will
    // not count this sample.
    bool key_pressed = true;
    switch (wgetch(display)) {
    case 'b': case 'B':
      s_key_display = DISPLAY_FLAT;
      break;
    case '#': case 's': case 'S':
      s_key_display = DISPLAY_SHARP;
      break;
    case ' ':
      sStatCounter.Reset();
      break;
    case 'c':
      kShowCount = !kShowCount;
      break;
    case 'p':
      paused = !paused;
      break;
    case KEY_DOWN:
      if (cent_threshold < 45) cent_threshold += 5;
      break;
    case KEY_UP:
      if (cent_threshold > 5) cent_threshold -= 5;
      break;
    case 'q':
      do_exit = true;
      break;
    case ERR:
      key_pressed = false;
      break;
    }
    if (key_pressed) {
      last_keypress_time = GetTime();
      any_change = true;
    }

    // No value 'heard', show statistics. Also, if we just pressed a key,
    // that might have created some noise we picked up; ignore that.
    if (paused || max_val < 2000 ||
        (last_keypress_time > 0 && last_keypress_time + 0.5 > GetTime())) {
      if (any_change) {
        print_stats(display, flat_pitch, sharp_pitch);
      }
      any_change = false;
    } else {
      double freq = dywapitch_computepitch(&tracker, analyze_buf);
      print_freq(freq, max_val, display, flat_pitch, sharp_pitch);
      any_change = true;
    }
  }
	
  endwin();
  snd_pcm_close(capture_handle);
  return 0;
}
