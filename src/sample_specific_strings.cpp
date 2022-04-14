/* matching_statistics - Computes the matching statistics from BWT and Thresholds
    Copyright (C) 2020 Massimiliano Rossi
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    You should have received a copy of the GNU General Public License
    along with this program.  If not, see http://www.gnu.org/licenses/ .
*/
/*!
   \file sample_specific.cpp
   \brief sample_specific.cpp Computes the matching statistics from BWT and Thresholds and extract the SFS.
   \author Massimiliano Rossi, Marco Oliva
   \date 14/04/2022
*/

extern "C" {
#include <xerrors.h>
}

#include <iostream>

#define VERBOSE

#include <common.hpp>

#include <sdsl/io.hpp>

#include <ms_pointers.hpp>

#include <malloc_count.h>

#include <SelfShapedSlp.hpp>
#include <DirectAccessibleGammaCode.hpp>
#include <SelectType.hpp>
#include <PlainSlp.hpp>
#include <FixedBitLenCode.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>


// for each
using ss_map_type = std::unordered_map<std::string, std::pair<std::size_t, std::unordered_set<std::size_t>>>;

struct mem_t
{
    size_t pos = 0;           // Position in the reference
    size_t len = 0;           // Length
    size_t idx = 0;           // Position in the pattern
    
    mem_t(size_t p, size_t l, size_t i)
    {
        pos = p; // Position in the reference
        len = l; // Length of the MEM
        idx = i; // Position in the read
    }
    
};

struct ss_type
{
    std::string seq;
    size_t l;
    size_t ref_pos;
    size_t read_pos;
};

////////////////////////////////////////////////////////////////////////////////
/// kseq extra
////////////////////////////////////////////////////////////////////////////////

static inline size_t ks_tell(kseq_t *seq)
{
  return gztell(seq->f->f) - seq->f->end + seq->f->begin;
}

void copy_kstring_t(kstring_t &l, kstring_t &r)
{
  l.l = r.l;
  l.m = r.m;
  l.s = (char *)malloc(l.m);
  for (size_t i = 0; i < r.m; ++i)
    l.s[i] = r.s[i];
}
void copy_kseq_t(kseq_t *l, kseq_t *r)
{
  copy_kstring_t(l->name, r->name);
  copy_kstring_t(l->comment, r->comment);
  copy_kstring_t(l->seq, r->seq);
  copy_kstring_t(l->qual, r->qual);
  l->last_char = r->last_char;
}

////////////////////////////////////////////////////////////////////////////////
/// Parallel computation
////////////////////////////////////////////////////////////////////////////////

// This should be done using buffering.
size_t next_start_fastq(gzFile fp)
{
  int c;
  // Special case when we arr at the beginning of the file.
  if ((gztell(fp) == 0) && ((c = gzgetc(fp)) != EOF) && c == '@')
    return 0;

  // Strart from the previous character
  gzseek(fp, -1, SEEK_CUR);

  std::vector<std::pair<int, size_t>> window;
  // Find the first new line
  for (size_t i = 0; i < 4; ++i)
  {
    while (((c = gzgetc(fp)) != EOF) && (c != (int)'\n'))
    {
    }
    if (c == EOF)
      return gztell(fp);
    if ((c = gzgetc(fp)) == EOF)
      return gztell(fp);
    window.push_back(std::make_pair(c, gztell(fp) - 1));
  }

  for (size_t i = 0; i < 2; ++i)
  {
    if (window[i].first == '@' && window[i + 2].first == '+')
      return window[i].second;
    if (window[i].first == '+' && window[i + 2].first == '@')
      return window[i + 2].second;
  }

  return gztell(fp);
}

// test if the file is gzipped
static inline bool is_gzipped(std::string filename)
{
  FILE *fp = fopen(filename.c_str(), "rb");
  if(fp == NULL) error("Opening file " + filename);
  int byte1 = 0, byte2 = 0;
  fread(&byte1, sizeof(char), 1, fp);
  fread(&byte2, sizeof(char), 1, fp);
  fclose(fp);
  return (byte1 == 0x1f && byte2 == 0x8b);
}

// Return the length of the file
// Assumes that the file is not compressed
static inline size_t get_file_size(std::string filename)
{
  if (is_gzipped(filename))
  {
    std::cerr << "The input is gzipped!" << std::endl;
    return -1;
  }
  FILE *fp = fopen(filename.c_str(), "r");
  fseek(fp, 0L, SEEK_END);
  size_t size = ftell(fp);
  fclose(fp);
  return size;
}

std::vector<size_t> split_fastq(std::string filename, size_t n_threads)
{
  //Precondition: the file is not gzipped
  // scan file for start positions and execute threads
  size_t size = get_file_size(filename);

  gzFile fp = gzopen(filename.c_str(), "r");
  if (fp == Z_NULL)
  {
    throw new std::runtime_error("Cannot open input file " + filename);
  }

  std::vector<size_t> starts(n_threads + 1);
  for (int i = 0; i < n_threads + 1; ++i)
  {
    size_t start = (size_t)((size * i) / n_threads);
    gzseek(fp, start, SEEK_SET);
    starts[i] = next_start_fastq(fp);
  }
  gzclose(fp);
  return starts;
}

////////////////////////////////////////////////////////////////////////////////
/// SLP definitions
////////////////////////////////////////////////////////////////////////////////

using SelSd = SelectSdvec<>;
using DagcSd = DirectAccessibleGammaCode<SelSd>;
using Fblc = FixedBitLenCode<>;

using shaped_slp_t = SelfShapedSlp<uint32_t, DagcSd, DagcSd, SelSd>;
using plain_slp_t = PlainSlp<uint32_t, Fblc, Fblc>;

template <typename slp_t>
std::string get_slp_file_extension()
{
  return std::string(".slp");
}

template <>
std::string get_slp_file_extension<shaped_slp_t>()
{
  return std::string(".slp");
}

template <>
std::string get_slp_file_extension<plain_slp_t>()
{
  return std::string(".plain.slp");
}
////////////////////////////////////////////////////////////////////////////////

template <typename slp_t>
class ms_c
{
public:

  ms_c(std::string filename)
  {
    verbose("Loading the matching statistics index");
    std::chrono::high_resolution_clock::time_point t_insert_start = std::chrono::high_resolution_clock::now();

    std::string filename_ms = filename + ms.get_file_extension();

    ifstream fs_ms(filename_ms);
    ms.load(fs_ms);
    fs_ms.close();

    std::chrono::high_resolution_clock::time_point t_insert_end = std::chrono::high_resolution_clock::now();

    verbose("Matching statistics index construction complete");
    verbose("Memory peak: ", malloc_count_peak());
    verbose("Elapsed time (s): ", std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count());

    verbose("Loading random access");
    t_insert_start = std::chrono::high_resolution_clock::now();

    std::string filename_slp = filename + get_slp_file_extension<slp_t>();

    ifstream fs(filename_slp);
    ra.load(fs);
    fs.close();

    n = ra.getLen();

    t_insert_end = std::chrono::high_resolution_clock::now();

    verbose("Matching statistics index loading complete");
    verbose("Memory peak: ", malloc_count_peak());
    verbose("Elapsed time (s): ", std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count());
  }

  // Destructor
  ~ms_c()
  {
      // NtD
  }

  // The outfile has the following format. The first size_t integer store the
  // length l of the query. Then the following l size_t integers stores the
  // pointers of the matching statistics, and the following l size_t integers
  // stores the lengths of the mathcing statistics.
  void matching_statistics(kseq_t *read, FILE* out, ss_map_type& sample_specifics, FILE* out_ss)
  {
    size_t mem_pos = 0;
    size_t mem_len = 0;
    size_t mem_idx = 0;
  
    auto pointers = ms.query(read->seq.s, read->seq.l);
    std::vector<size_t> lengths(pointers.size());
    size_t l = 0;
    size_t n_Ns = 0;
    for (size_t i = 0; i < pointers.size(); ++i)
    {
      size_t pos = pointers[i];
      while ((i + l) < read->seq.l && (pos + l) < n && read->seq.s[i + l] == ra.charAt(pos + l))
      {
        if (read->seq.s[i + l] == 'N')
          n_Ns++;
        else
          n_Ns = 0;
        ++l;
      }
    
      lengths[i] = l;
      l = (l == 0 ? 0 : (l - 1));
    
      // Update MEM
      if (lengths[i] > mem_len and n_Ns < lengths[i])
      {
        mem_len = lengths[i];
        mem_pos = pointers[i];
        mem_idx = i;
      }
    }
  
    mem_t read_longest_mem(mem_pos, mem_len, mem_idx);

    // Original MS computation
    // for (size_t i = 0; i < pointers.size(); ++i)
    // {
    //   size_t pos = pointers[i];
    //   while ((i + l) < read->seq.l && (pos + l) < n && read->seq.s[i + l] == ra.charAt(pos + l))
    //     ++l;

    //   lengths[i] = l;
    //   l = (l == 0 ? 0 : (l - 1));
    // }

    assert(lengths.size() == pointers.size());
    
    // compute S_t
    std::vector<ss_type> specific_string_per_read;
    for (std::size_t i = 1; i < lengths.size(); i++)
    {
        if (lengths[i] >= lengths[i - 1])
        {
            std::string sample_specific;
            for (std::size_t j = i - 1; j <= i + lengths[i - 1]; j++) { sample_specific += read->seq.s[j]; }
            if (sample_specific.size() != 0)
            {
                sample_specifics[sample_specific].first += 1;
                sample_specifics[sample_specific].second.insert(pointers[i-1]);
                
                ss_type ssspr;
                ssspr.seq = sample_specific;
                ssspr.l = sample_specific.size();
                ssspr.read_pos = i - 1;
                ssspr.ref_pos = pointers[i - 1];
    
                specific_string_per_read.push_back(ssspr);
            }
        }
    }
    
    size_t q_length = pointers.size();
    fwrite(&q_length, sizeof(size_t), 1,out);
    fwrite(pointers.data(), sizeof(size_t),q_length,out);
    fwrite(lengths.data(), sizeof(size_t),q_length,out);
    
    // write sample specifics per read
    fwrite(&(read->name.l), sizeof(size_t), 1, out_ss);
    fwrite(read->name.s, sizeof(char), read->name.l, out_ss);
    fwrite(&(read_longest_mem.pos), sizeof(size_t), 1, out_ss);
    fwrite(&(read_longest_mem.idx), sizeof(size_t), 1, out_ss);
    fwrite(&(read_longest_mem.len), sizeof(size_t), 1, out_ss);
    size_t sss_length = specific_string_per_read.size();
    std::cout << read->name.s << " " << sss_length << std::endl;
    fwrite(&sss_length, sizeof(size_t), 1, out_ss);
    for (auto& sss : specific_string_per_read)
    {
        fwrite(&(sss.l), sizeof(size_t), 1, out_ss);
        fwrite(sss.seq.c_str(), sizeof(char), sss.l, out_ss);
        fwrite(&(sss.read_pos), sizeof(size_t), 1, out_ss);
        fwrite(&(sss.ref_pos), sizeof(size_t), 1, out_ss);
    }
    
  }

protected:
  ms_pointers<> ms;
  slp_t ra;
  size_t n = 0;
};



char complement(char n)
{
  switch (n)
  {
  case 'A':
    return 'T';
  case 'T':
    return 'A';
  case 'G':
    return 'C';
  case 'C':
    return 'G';
  default:
    return n;
  }
}

template <typename ms_t>
struct mt_param_t
{
  // Parameters
  ms_t *ms;
  std::string pattern_filename;
  std::string out_filename;
  std::string out_ss_filename;
  size_t start;
  size_t end;
  size_t wk_id;
  ss_map_type sample_specifics;
};

template <typename ms_t>
void *mt_ms_worker(void *param)
{
  mt_param_t<ms_t> *p = (mt_param_t<ms_t>*) param;
  size_t n_reads = 0;
  size_t n_aligned_reads = 0;

  FILE *out_fd, *out_sss_pr;
  gzFile fp;

  if ((out_fd = fopen(p->out_filename.c_str(), "w")) == nullptr)
    error("open() file " + p->out_filename + " failed");
  
  if ((out_sss_pr = fopen(p->out_ss_filename.c_str(), "w")) == nullptr)
    error("open() file " + p->out_ss_filename + " failed");

  if ((fp = gzopen(p->pattern_filename.c_str(), "r")) == Z_NULL)
    error("open() file " + p->pattern_filename + " failed");

  gzseek(fp, p->start, SEEK_SET);

  kseq_t rev;
  int l;

  kseq_t *seq = kseq_init(fp);
  while ((ks_tell(seq) < p->end) && ((l = kseq_read(seq)) >= 0))
  {
    p->ms->matching_statistics(seq,out_fd, p->sample_specifics, out_sss_pr);
  }

  kseq_destroy(seq);
  gzclose(fp);
  fclose(out_fd);
  fclose(out_sss_pr);
  
  return NULL;
}

template <typename ms_t>
void mt_ms( ms_t *ms, std::string pattern_filename, std::string out_filename, size_t n_threads)
{
  pthread_t t[n_threads] = {0};
  mt_param_t<ms_t> params[n_threads];
  std::vector<size_t> starts = split_fastq(pattern_filename, n_threads);
  for(size_t i = 0; i < n_threads; ++i)
  {
    params[i].ms = ms;
    params[i].pattern_filename = pattern_filename;
    params[i].out_filename = out_filename + "_" + std::to_string(i) + ".ms.tmp.out";
    params[i].out_ss_filename = out_filename + "_" + std::to_string(i) + ".ss.tmp.out";
    params[i].start = starts[i];
    params[i].end = starts[i+1];
    params[i].wk_id = i;
    xpthread_create(&t[i], NULL, &mt_ms_worker<ms_t>, &params[i], __LINE__, __FILE__);
  }

  for(size_t i = 0; i < n_threads; ++i)
  {
    xpthread_join(t[i],NULL,__LINE__,__FILE__);
  }
  
  // Merge sample specifics
  ss_map_type sample_specifics;
  for(size_t i = 0; i < n_threads; ++i)
  {
      for (auto& sample_specific_pair : params[i].sample_specifics)
      {
          sample_specifics[sample_specific_pair.first].first += sample_specific_pair.second.first;
          sample_specifics[sample_specific_pair.first].second.insert(
                    sample_specific_pair.second.second.begin(),
                    sample_specific_pair.second.second.end());
      }
  }
  
  std::string sss_filename = out_filename + ".sss";
  FILE *out_sss;
  if ((out_sss = fopen(sss_filename.c_str(), "w")) == nullptr)
      error("open() file " + sss_filename + " failed");
  
  for (auto& sample_specific_pair : sample_specifics)
  {
     std::size_t string_size = sample_specific_pair.first.size();
     fwrite(&string_size, sizeof(std::size_t), 1, out_sss);
     fwrite(sample_specific_pair.first.c_str(), sizeof(char), sample_specific_pair.first.size(), out_sss);
     fwrite(&(sample_specific_pair.second.first), sizeof(std::size_t), 1, out_sss);
     std::size_t positions_size = sample_specific_pair.second.second.size();
     fwrite(&positions_size, sizeof(std::size_t), 1, out_sss);
     for (auto& pos : sample_specific_pair.second.second) { fwrite(&pos, sizeof(std::size_t), 1, out_sss); }
  }
  
  fclose(out_sss);

  sleep(5);
  
  return;
}


////////////////////////////////////////////////////////////////////////////////
/// Single Thread
////////////////////////////////////////////////////////////////////////////////
template <typename ms_t>
size_t st_ms(ms_t *ms, std::string pattern_filename, std::string out_filename)
{
  size_t n_reads = 0;
  size_t n_aligned_reads = 0;
  kseq_t rev;
  int l;
  FILE *out_fd, *out_sss, *out_sss_pr;
  
  ss_map_type specific_strings;
  
  std::string sss_filename = out_filename + ".sss";
  std::string sss_pr_filename =  out_filename + "_0.ss.tmp.out";
  out_filename += "_0.ms.tmp.out";
  

  if ((out_fd = fopen(out_filename.c_str(), "w")) == nullptr)
    error("open() file " + out_filename + " failed");
  if ((out_sss_pr = fopen(sss_pr_filename.c_str(), "w")) == nullptr)
    error("open() file " + sss_pr_filename + " failed");
  
  if ((out_sss = fopen(sss_filename.c_str(), "w")) == nullptr)
    error("open() file " + sss_filename + " failed");

  gzFile fp = gzopen(pattern_filename.c_str(), "r");
  kseq_t* seq = kseq_init(fp);
  while ((l = kseq_read(seq)) >= 0)
  {
    ms->matching_statistics(seq, out_fd, specific_strings, out_sss_pr);
  }
  
  for (auto& sample_specific_pair : specific_strings)
  {
      std::size_t string_size = sample_specific_pair.first.size();
      fwrite(&string_size, sizeof(std::size_t), 1, out_sss);
      fwrite(sample_specific_pair.first.c_str(), sizeof(char), sample_specific_pair.first.size(), out_sss);
      fwrite(&(sample_specific_pair.second.first), sizeof(std::size_t), 1, out_sss);
      std::size_t positions_size = sample_specific_pair.second.second.size();
      fwrite(&positions_size, sizeof(std::size_t), 1, out_sss);
      for (auto& pos : sample_specific_pair.second.second) { fwrite(&pos, sizeof(std::size_t), 1, out_sss); }
  }

  kseq_destroy(seq);
  gzclose(fp);
  fclose(out_fd);
  fclose(out_sss);
  fclose(out_sss_pr);

  sleep(5);

  return n_aligned_reads;
}


typedef std::pair<std::string, std::vector<uint8_t>> pattern_t;

//*********************** Argument options ***************************************
// struct containing command line parameters and other globals
struct Args
{
  std::string filename = "";
  size_t w = 10;             // sliding window size and its default
  bool store = false;        // store the data structure in the file
  bool memo = false;         // print the memory usage
  bool csv = false;          // print stats on stderr in csv format
  bool rle = false;          // outpt RLBWT
  std::string patterns = ""; // path to patterns file
  size_t l = 25;             // minumum MEM length
  size_t th = 1;             // number of threads
  bool is_fasta = false;     // read a fasta file
  bool shaped_slp = false;   // use shaped slp
};

void parseArgs(int argc, char *const argv[], Args &arg)
{
  int c;
  extern char *optarg;
  extern int optind;

  std::string usage("usage: " + std::string(argv[0]) + " infile [-s store] [-m memo] [-c csv] [-p patterns] [-f fasta] [-r rle] [-t threads] [-l len] [-q shaped_slp]\n\n" +
                    "Computes the pfp data structures of infile, provided that infile.parse, infile.dict, and infile.occ exists.\n" +
                    "     wsize: [integer] - sliding window size (def. 10)\n" +
                    "     store: [boolean] - store the data structure in infile.pfp.ds. (def. false)\n" +
                    "      memo: [boolean] - print the data structure memory usage. (def. false)\n" +
                    "     fasta: [boolean] - the input file is a fasta file. (def. false)\n" +
                    "       rle: [boolean] - output run length encoded BWT. (def. false)\n" +
                    "shaped_slp: [boolean] - use shaped slp. (def. false)\n" +
                    "   pattens: [string]  - path to patterns file.\n" +
                    "       len: [integer] - minimum MEM lengt (def. 25)\n" +
                    "    thread: [integer] - number of threads (def. 1)\n" +
                    "       csv: [boolean] - print the stats in csv form on strerr. (def. false)\n");

  std::string sarg;
  while ((c = getopt(argc, argv, "w:smcfql:rhp:t:")) != -1)
  {
    switch (c)
    {
    case 'w':
      sarg.assign(optarg);
      arg.w = stoi(sarg);
      break;
    case 's':
      arg.store = true;
      break;
    case 'm':
      arg.memo = true;
      break;
    case 'c':
      arg.csv = true;
      break;
    case 'r':
      arg.rle = true;
      break;
    case 'p':
      arg.patterns.assign(optarg);
      break;
    case 'l':
      sarg.assign(optarg);
      arg.l = stoi(sarg);
      break;
    case 't':
      sarg.assign(optarg);
      arg.th = stoi(sarg);
      break;
    case 'f':
      arg.is_fasta = true;
      break;
    case 'q':
      arg.shaped_slp = true;
      break;
    case 'h':
      error(usage);
    case '?':
      error("Unknown option.\n", usage);
      exit(1);
    }
  }
  // the only input parameter is the file name
  if (argc == optind + 1)
  {
    arg.filename.assign(argv[optind]);
  }
  else
  {
    error("Invalid number of arguments\n", usage);
  }
}

//********** end argument options ********************

template <typename ms_t>
void dispatcher(Args &args)
{
  verbose("Construction of the matching statistics data structure");
  std::chrono::high_resolution_clock::time_point t_insert_start = std::chrono::high_resolution_clock::now();
  
  ms_t ms(args.filename);

  std::chrono::high_resolution_clock::time_point t_insert_end = std::chrono::high_resolution_clock::now();
  verbose("Memory peak: ", malloc_count_peak());
  verbose("Elapsed time (s): ", std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count());

  verbose("Processing patterns");
  t_insert_start = std::chrono::high_resolution_clock::now();

  std::string base_name = basename(args.filename.data());
  std::string out_filename = args.patterns + "_" + base_name;

  if (is_gzipped(args.patterns))
  {
    verbose("The input is gzipped - forcing single thread matchin statistics.");
    args.th = 1;
  }

  if (args.th == 1)
    st_ms<ms_t>(&ms, args.patterns, out_filename);
  else
    mt_ms<ms_t>(&ms, args.patterns, out_filename, args.th);

  // TODO: Merge the SAM files.

  t_insert_end = std::chrono::high_resolution_clock::now();

  verbose("Memory peak: ", malloc_count_peak());
  verbose("Elapsed time (s): ", std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count());

  auto mem_peak = malloc_count_peak();
  verbose("Memory peak: ", malloc_count_peak());

  verbose("Printing plain output");
  t_insert_start = std::chrono::high_resolution_clock::now();

  std::ofstream f_pointers(out_filename + ".pointers");
  std::ofstream f_lengths(out_filename + ".lengths");

  if (!f_pointers.is_open())
    error("open() file " + std::string(out_filename) + ".pointers failed");

  if (!f_lengths.is_open())
    error("open() file " + std::string(out_filename) + ".lengths failed");

  size_t n_seq = 0;
  for (size_t i = 0; i < args.th; ++i)
  {
    std::string tmp_filename = out_filename + "_" + std::to_string(i) + ".ms.tmp.out";
    FILE *in_fd;

    if ((in_fd = fopen(tmp_filename.c_str(), "r")) == nullptr)
      error("open() file " + tmp_filename + " failed");

    size_t length = 0;
    size_t m = 100; // Reserved size for pointers and lengths
    size_t *mem = (size_t *)malloc(m * sizeof(size_t));
    while (!feof(in_fd) and fread(&length, sizeof(size_t), 1, in_fd) > 0)
    {
      if (m < length)
      {
        // Resize lengths and pointers
        m = length;
        mem = (size_t *)realloc(mem, m * sizeof(size_t));
      }

      if ((fread(mem, sizeof(size_t), length, in_fd)) != length)
        error("fread() file " + std::string(tmp_filename) + " failed");

      // TODO: Store the fasta headers somewhere
      f_pointers << ">" + std::to_string(n_seq) << endl;
      for (size_t i = 0; i < length; ++i)
        f_pointers << mem[i] << " ";
      f_pointers << endl;

      if ((fread(mem, sizeof(size_t), length, in_fd)) != length)
        error("fread() file " + std::string(tmp_filename) + " failed");

      f_lengths << ">" + std::to_string(n_seq) << endl;
      for (size_t i = 0; i < length; ++i)
        f_lengths << mem[i] << " ";
      f_lengths << endl;

      n_seq++;
    }
    fclose(in_fd);
  }

  f_pointers.close();
  f_lengths.close();

  t_insert_end = std::chrono::high_resolution_clock::now();

  verbose("Memory peak: ", malloc_count_peak());
  verbose("Elapsed time (s): ", std::chrono::duration<double, std::ratio<1>>(t_insert_end - t_insert_start).count());

  mem_peak = malloc_count_peak();
  verbose("Memory peak: ", malloc_count_peak());

  size_t space = 0;
  if (args.memo)
  {
  }

  if (args.store)
  {
  }

  if (args.csv)
    std::cerr << csv(args.filename.c_str(), time, space, mem_peak) << std::endl;
}

int main(int argc, char *const argv[])
{
  Args args;
  parseArgs(argc, argv, args);

  if (args.shaped_slp)
  {
    dispatcher<ms_c<shaped_slp_t>>(args);
  }
  else
  {
    dispatcher<ms_c<plain_slp_t>>(args);
  }
  return 0;
}