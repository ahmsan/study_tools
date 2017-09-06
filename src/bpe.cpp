#include <stdio.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <string>
#include <map>
#include <unistd.h>
#include <iconv.h>
#include <errno.h>
#include <stdint.h>

using namespace std;
const uint16_t g_wp_id_bit = 17;
uint32_t g_total_wp_freq[(uint64_t)1<<(2*g_wp_id_bit)];

class CharsetConverter
{
public:
  CharsetConverter(string charset_from, string charset_to) {
    from_charset_ = charset_from;
    to_charset_ = charset_to;
    cd_ = iconv_open(to_charset_.c_str(), from_charset_.c_str()); 
  }

  ~CharsetConverter() {
    if ( cd_ > 0 ) {
      iconv_close(cd_);
      cd_ = 0;
    }
  }

  int Convert(char *inbuf, size_t inlen, char *outbuf, size_t *outlen) {
    if (cd_ <= 0 ) {
      printf("invalid conversion descriptor: %d\n", cd_);
      return -1;
    } 

    char **pin = &inbuf;
    char **pout = &outbuf;
    memset(outbuf, 0, *outlen);
    size_t outlenleft = *outlen;
    size_t ret = iconv(cd_, pin, &inlen, pout, &outlenleft);
    if ( ret != (size_t)-1 ) {
      *outlen = *outlen - outlenleft; 
      ret = 0;
    }

    return ret;
  }

private:
  string from_charset_;
  string to_charset_;
  iconv_t cd_;
};

class BPEAlgo
{
public:
  BPEAlgo(string &word_freq_file, int dict_size) {
    word_freq_file_ = word_freq_file;
    dict_size_ = dict_size;
    underscore_ = "_" + string(1, (char)0);
  }

  ~BPEAlgo() {
    id2piece_.clear();
    piece2id_.clear();
    for ( int i = 0; i < wp_freq_.size(); ++i ) {
      wp_freq_[i].first.clear();
    }
  }

  int Init() {
    CharsetConverter charset_conv(BPEAlgo::s_charset_from_, BPEAlgo::s_charset_to_);
    FILE *fp = fopen(word_freq_file_.c_str(), "r");
    if (fp == NULL) {
      printf("open file: %s failed!\n", word_freq_file_.c_str()); 
      return -2;
    }

    char buff[512];
    int handled_line = 0;
    while ( fgets(buff, sizeof(buff), fp) ) {
      handled_line += 1;
      char word[512] = { 0 };
      int id = 0; 
      uint32_t freq = 0;

      if ( buff[0] == 0 ) {
        continue;
      }
      if ( sscanf(buff, "%s %d %u", word, &id, &freq) != 3 ) {
        continue;
      }

      char outbuf[1024];
      size_t outlen = sizeof(outbuf);
      int ret = charset_conv.Convert(word, strlen(word), outbuf, &outlen); 
      if ( ret == 0 ) {
        int i = handled_line == 1 ? 2 : 0;
        int32_t wp_id = 0;
        vector<int32_t> wp_ids;
        bool new_start = true;
        for ( ; i < outlen; i += 2 ) {
          string stmp(&outbuf[i], 2);
          if ( new_start && IsPrintable(stmp.c_str()) ) {
            wp_id = AddPiece(underscore_ + stmp);
            new_start = false;
          } else {
            wp_id = AddPiece(stmp);
          }
          wp_ids.push_back(wp_id);
        }
        wp_freq_.push_back(make_pair(wp_ids, freq));
      }
    }
    return 0;
  }

  int DoBPEAlgo() {
    for ( int i = 0; i < dict_size_; ++i ) {
      int32_t prefix = 0;
      int32_t suffix = 0;
      if ( GetBestOne(prefix, suffix) != 0 ) {
        break;
      }
      if ( prefix >= 0 && suffix >= 0 && prefix < id2piece_.size() && suffix < id2piece_.size() ) {
        string wp = id2piece_[prefix] + id2piece_[suffix];
        int32_t wp_id = AddPiece(wp);
        Replace(wp_id, prefix, suffix);
        ShowBestOne(wp, id2piece_[prefix], id2piece_[suffix]);
      } else {
        string wp = id2piece_[prefix] + id2piece_[suffix];
        ShowBestOne(wp, id2piece_[prefix], id2piece_[suffix]); 
      }
    }
    return 0;
  }

private:
  int GetBestOne(int &prefix, int &suffix) {
    vector<uint64_t> wp_idx;
    wp_idx.reserve(100000);
    for ( int i = 0; i < wp_freq_.size(); ++i ) {
      vector<int32_t> &wp_ids = wp_freq_[i].first;
      uint32_t curr_freq = wp_freq_[i].second;
      for ( int j = 0; j+1 < wp_ids.size() && wp_ids[j+1] >= 0; ++j ) {
        uint64_t neig_wp = (((uint64_t)wp_ids[j]) << g_wp_id_bit) + wp_ids[j+1];
        uint32_t &total_freq = g_total_wp_freq[neig_wp];
        if ( total_freq == 0 ) {
          wp_idx.push_back(neig_wp);
        }
        total_freq += curr_freq;
      }
    } 
    uint64_t neig_ids = 0;
    uint32_t max_total_freq = 0;
    for ( int i = 0; i < wp_idx.size(); ++i ) {
      uint32_t &wp_freq = g_total_wp_freq[wp_idx[i]];
      if ( wp_freq > max_total_freq ) {
        max_total_freq = wp_freq;
        neig_ids = wp_idx[i];
      }
      wp_freq = 0;
    }
  
    if ( wp_idx.size() < 1 ) {
      return -1;
    }

    prefix = neig_ids >> g_wp_id_bit;
    suffix = neig_ids & (((uint64_t)1<<g_wp_id_bit) - 1);
    return 0;
  }
  
  void Replace(int new_id, int from, int to) {
    //printf("prefix: %d, suffix: %d to: %d, idsize: %d\n", from ,to, new_id, id2piece_.size() );
    for ( int i = 0; i < wp_freq_.size(); ++i ) {
      int k = 0;
      vector<int32_t> &wp_ids = wp_freq_[i].first;
      for (int j = 0; j < wp_ids.size() && wp_ids[j] >= 0; ++j ) {
        if ( j+1 < wp_ids.size() && wp_ids[j] == from && wp_ids[j+1] == to ) {
          wp_ids[k++] = new_id;
          ++j;
        } else {
          wp_ids[k++] = wp_ids[j];
        }
      }
      if ( k < wp_ids.size() && wp_ids[k] >= 0 ) {
        wp_ids[k] = -1;
      }
    }
  }

  int ShowBestOne(string &t, string &pstr, string &sstr) {
    CharsetConverter charset_conv(BPEAlgo::s_charset_to_, BPEAlgo::s_charset_from_);
    char o[1024] = { 0 };
    int k = 0;
    o[k++] = -1;
    o[k++] = -2;
    memcpy(&o[k], t.c_str(), t.size());
    k += t.size();
    o[k++] = ' ';
    o[k++] = 0;
    memcpy(&o[k], pstr.c_str(), pstr.size());
    k += pstr.size();
    o[k++] = ' ';
    o[k++] = 0;
    memcpy(&o[k], sstr.c_str(), sstr.size());
    k += sstr.size();
    o[k++] = 0;
    o[k++] = 0;

    char outbuf[1024];
    size_t outlen = sizeof(outbuf);
    int ret = charset_conv.Convert(o, k, outbuf, &outlen); 
    printf("%s\n", outbuf);
    fflush(stdout);
  }

  int32_t AddPiece(const string &piece) {
    if ( piece2id_.find(piece) != piece2id_.end() ) {
      return piece2id_[piece];
    }
    piece2id_[piece] = id2piece_.size();
    id2piece_.push_back(piece);
    return id2piece_.size() - 1;
  }

  int IsPrintable(const char* unicode) {
    return unicode && unicode[0] > 0 && unicode[0] < 256 and unicode[1] == 0; 
  }

private:
  int dict_size_;
  string underscore_;
  string word_freq_file_;
  vector<string> id2piece_;
  map<string, int32_t> piece2id_;
  vector<pair<vector<int32_t>, uint32_t> > wp_freq_;

private:
  const static char* s_charset_from_;
  const static char* s_charset_to_;
};

const  char* BPEAlgo::s_charset_from_ = "utf-8";
const  char* BPEAlgo::s_charset_to_ = "UNICODE//IGNORE";


int main(int argc, char* argv[]) {
  if ( argc < 3 ) {
    printf("usage: %s voc-file dict-cnt\n", argv[0]);
    return -1;
  }

  string word_freq_file = argv[1];
  int dict_size = atoi(argv[2]);
  if ( dict_size >= 1<<16 ) { 
    printf("dict size max: 2^16-1\n");
    return -1; 
  }

  BPEAlgo ba(word_freq_file, dict_size);
  int ret = ba.Init();
  if ( ret != 0 ) {
    printf("init failed, ret: %d\n", ret);
    return -1;
  }

  return ba.DoBPEAlgo();
}

