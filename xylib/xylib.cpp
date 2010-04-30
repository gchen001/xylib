// Implementation of Public API of xylib library.
// Licence: Lesser GNU Public License 2.1 (LGPL)
// $Id$

#define BUILDING_XYLIB
#include "xylib.h"

#include <cassert>
#include <cstring>
#include <iomanip>
#include <algorithm>

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#ifdef HAVE_ZLIB
#  include <zlib.h>
#endif

#ifdef HAVE_BZLIB
#  include <bzlib.h>
#endif

#include "util.h"
#include "brucker_raw.h"
#include "rigaku_dat.h"
#include "text.h"
#include "uxd.h"
#include "vamas.h"
#include "philips_udf.h"
#include "winspec_spe.h"
#include "pdcif.h"
#include "philips_raw.h"
//#include "gsas.h"
#include "cpi.h"
#include "dbws.h"
#include "canberra_mca.h"
#include "xfit_xdd.h"
#include "riet7.h"

#include <vector>
#include <map>

using namespace std;
using namespace xylib;
using namespace xylib::util;

const FormatInfo *formats[] = {
    &CpiDataSet::fmt_info,
    &UxdDataSet::fmt_info,
    &RigakuDataSet::fmt_info,
    &BruckerRawDataSet::fmt_info,
    &VamasDataSet::fmt_info,
    &UdfDataSet::fmt_info,
    &WinspecSpeDataSet::fmt_info,
    &PdCifDataSet::fmt_info,
    &PhilipsRawDataSet::fmt_info,
    //&GsasDataSet::fmt_info,
    &CanberraMcaDataSet::fmt_info,
    &XfitXddDataSet::fmt_info,
    &Riet7DataSet::fmt_info,
    &DbwsDataSet::fmt_info,
    &TextDataSet::fmt_info,
    NULL // it must be a NULL-terminated array
};

// implementation of C API
extern "C" {

const struct xylibFormat* xylib_get_format(int n)
{
    if (n < 0 || (size_t) n >= sizeof(formats) / sizeof(formats[0]))
        return NULL;
    return formats[n];
}

const struct xylibFormat* xylib_get_format_by_name(const char* name)
{
    for (FormatInfo const **i = formats; *i != NULL; ++i)
        if (strcmp(name, (*i)->name) == 0)
            return *i;
    return NULL;
}

/// see also XYLIB_VERSION
const char* xylib_get_version()
{
    static bool initialized = false;
    static char ver[16];
    if (!initialized) {
        sprintf(ver, "%d.%d.%d", XYLIB_VERSION / 10000,
                                 XYLIB_VERSION / 100 % 100,
                                 XYLIB_VERSION % 100);
        initialized = true;
    }
    return ver;
}

void* xylib_load_file(const char* path, const char* format_name,
                      const char* options)
{
    try {
        return (void*) load_file(path, format_name != NULL ? format_name : "",
                                       options != NULL ? options : "");
    }
    catch (std::exception&) {
        return NULL;
    }
}

void* xylib_get_block(void* dataset, int block)
{
    try {
        return (void*) ((DataSet*) dataset)->get_block(block);
    }
    catch (RunTimeError&) {
        return NULL;
    }
}

int xylib_count_columns(void* block)
{
    return ((Block*) block)->get_column_count();
}

int xylib_count_rows(void* block, int column)
{
    if (column < 0 || column > xylib_count_columns(block))
        return 0;
    return ((Block*) block)->get_column(column).get_point_count();
}

double xylib_get_data(void* block, int column, int row)
{
    return ((Block*) block)->get_column(column).get_value(row);
}

const char* xylib_dataset_metadata(void* dataset, const char* key)
{
    try {
        return ((DataSet*) dataset)->meta.get(key).c_str();
    }
    catch (RunTimeError&) {
        return NULL;
    }
}

const char* xylib_block_metadata(void* block, const char* key)
{
    try {
        return ((Block*) block)->meta.get(key).c_str();
    }
    catch (RunTimeError&) {
        return NULL;
    }
}

void xylib_free_dataset(void* dataset)
{
    delete (DataSet*) dataset;
}

} // extern "C"

namespace xylib {

FormatInfo::FormatInfo(const char* name_, const char* desc_, const char* exts_,
                       bool binary_, bool multiblock_,
                       t_ctor ctor_, t_checker checker_)
{
        name = name_;
        desc = desc_;
        exts = exts_;
        binary = (int) binary_;
        multiblock = (int) multiblock_;
        ctor = ctor_;
        checker = checker_;
}

bool check_format(FormatInfo const* fi, std::istream& f)
{
    return !fi->checker || (*fi->checker)(f);
}

struct MetaDataImp : public map<string, string>
{
};

MetaData::MetaData()
    : imp_(new MetaDataImp)
{
}

MetaData::~MetaData()
{
    delete imp_;
}

void MetaData::operator=(const MetaData& other)
{
    *imp_ = *other.imp_;
}

bool MetaData::has_key(std::string const& key) const
{
    return imp_->find(key) != imp_->end();
}

string const& MetaData::get(string const& key) const
{
    map<string,string>::const_iterator it = imp_->find(key);
    if (it == imp_->end())
        throw RunTimeError("no such key in meta-info found");
    return it->second;
}

bool MetaData::set(string const& key, string const& val)
{
    //map::insert returns pair<iterator,bool>
    return imp_->insert(make_pair(key, val)).second;
}

size_t MetaData::size() const
{
    return imp_->size();
}

string const& MetaData::get_key(size_t index) const
{
    map<string,string>::const_iterator it = imp_->begin();
    for (size_t i = 0; i < index; ++i)
        ++it;
    return it->first;
}

void MetaData::clear()
{
    imp_->clear();
}

string& MetaData::operator[] (string const& x)
{
    return (*imp_)[x];
}


Column* const Block::index_column = new StepColumn(0, 1);

struct BlockImp
{
    string name;
    vector<Column*> cols;
};

Block::Block()
    : imp_(new BlockImp)
{
}

Block::~Block()
{
    for (vector<Column*>::iterator it = imp_->cols.begin();
                                                it != imp_->cols.end(); ++it)
        delete *it;
    delete imp_;
}

std::string const& Block::get_name() const
{
    return imp_->name;
}

void Block::set_name(std::string const& name)
{
    imp_->name = name;
}

int Block::get_column_count() const
{
    return (int) imp_->cols.size();
}

const Column& Block::get_column(int n) const
{
    if (n == 0)
        return *index_column;
    int c = (n < 0 ? n + (int) imp_->cols.size() : n - 1);
    if (c < 0 || c >= (int) imp_->cols.size())
        throw RunTimeError("column index out of range: " + S(n));
    return *imp_->cols[c];
}

void Block::add_column(Column* c, bool append)
{
    imp_->cols.insert((append ? imp_->cols.end() : imp_->cols.begin()), c);
}

void Block::del_column(int n)
{
    imp_->cols.erase(imp_->cols.begin() + n);
}

int Block::get_point_count() const
{
    int min_n = -1;
    for (vector<Column*>::const_iterator i = imp_->cols.begin();
                                                i != imp_->cols.end(); ++i) {
        int n = (*i)->get_point_count();
        if (min_n == -1 || (n != -1 && n < min_n))
            min_n = n;
    }
    return min_n;
}

struct DataSetImp
{
    std::vector<Block*> blocks;
    std::string options;
};

DataSet::DataSet(FormatInfo const* fi_)
    : fi(fi_), imp_(new DataSetImp)
{
}

DataSet::~DataSet()
{
    clear();
    delete imp_;
}

int DataSet::get_block_count() const
{
    return (int) imp_->blocks.size();
}

const Block* DataSet::get_block(int n) const
{
    if (n < 0 || (size_t)n >= imp_->blocks.size())
        throw RunTimeError("no block #" + S(n) + " in this file.");
    return imp_->blocks[n];
}

// clear all the data of this dataset
void DataSet::clear()
{
    for (vector<Block*>::iterator i = imp_->blocks.begin();
            i != imp_->blocks.end(); ++i)
        delete *i;
    imp_->blocks.clear();
    meta.clear();
}

bool DataSet::has_option(string const& t)
{
    return has_word(imp_->options, t);
}

void DataSet::add_block(Block* block)
{
    imp_->blocks.push_back(block);
}

void DataSet::set_options(string const& options)
{
    imp_->options = options;
}


DataSet* load_stream_of_format(istream &is, FormatInfo const* fi,
                               string const& options)
{
    assert(fi != NULL);
    // check if the file is not empty
    is.peek();
    if (is.eof())
        throw FormatError("The file is empty.");

    DataSet *ds = (*fi->ctor)();
    ds->set_options(options);
    ds->load_data(is);
    return ds;
}


// One pass input streambuf. It reads and decompress whole file in ctor.
struct decompressing_istreambuf : public std::streambuf
{
    decompressing_istreambuf() { init_buf(); }

    void init_buf()
    {
        bufavail_ = 512;
        bufdata_ = (char*) malloc(bufavail_);
        writeptr_ = bufdata_;
    }

    // should be called only when the buffer is full, double the buffer size 
    void double_buf()
    {
        int old_size = (int) (writeptr_ - bufdata_);
        bufdata_ = (char*) realloc(bufdata_, 2 * old_size);
        if (!bufdata_)
            throw RunTimeError("Can't allocate memory (" + S(2*old_size)
                    + " bytes).");
        // bufdata_ can be changed
        writeptr_ = bufdata_ + old_size;
        bufavail_ = old_size;
    }

    ~decompressing_istreambuf() { free(bufdata_); }

protected:
    int bufavail_;
    char* bufdata_;
    char* writeptr_;
};

#ifdef HAVE_ZLIB
struct gzip_istreambuf : public decompressing_istreambuf
{
    gzip_istreambuf(gzFile gz)
    {
        for (;;) {
            int n = gzread(gz, writeptr_, bufavail_);
            writeptr_ += n;
            if (n != bufavail_)
                break;
            double_buf();
        }
        setg(bufdata_, bufdata_, writeptr_);
    }
};
#endif

#ifdef HAVE_BZLIB
struct bzip2_istreambuf : public decompressing_istreambuf
{
    bzip2_istreambuf(BZFILE* bz2)
    {
        for (;;) {
            int n = BZ2_bzread(bz2, writeptr_, bufavail_);//the only difference
            writeptr_ += n;
            if (n != bufavail_)
                break;
            double_buf();
        }
        setg(bufdata_, bufdata_, writeptr_);
    }
};
#endif


DataSet* guess_and_load_stream(istream &is,
                               string const& path, // only used for guessing
                               string const& format_name,
                               string const& options)
{
    FormatInfo const* fi = NULL;
    if (format_name.empty()) {
        fi = guess_filetype(path, is);
        if (!fi)
            throw RunTimeError ("Format of the file can not be guessed");
        is.seekg(0);
        is.clear();
    }
    else {
        fi = (FormatInfo const*) xylib_get_format_by_name(format_name.c_str());
        if (!fi)
            throw RunTimeError("Unsupported (misspelled?) data format: "
                                + format_name);
    }

    return load_stream_of_format(is, fi, options);
}


DataSet* load_file(string const& path, string const& format_name,
                   string const& options)
{
    DataSet *ret = NULL;
    // open stream
    int len = (int) path.size();
    bool gzipped = (len > 3 && path.substr(len-3) == ".gz");
    bool bz2ed = (len > 4 && path.substr(len-4) == ".bz2");
    if (gzipped) {
#ifdef HAVE_ZLIB
        gzFile gz_stream = gzopen(path.c_str(), "rb");
        if (!gz_stream) {
            throw RunTimeError("can't open .gz input file: " + path);
        }
        gzip_istreambuf istrbuf(gz_stream);
        istream is(&istrbuf);
        ret = guess_and_load_stream(is, path.substr(0, len-3),
                                    format_name, options);
#else
        throw RunTimeError("Program is compiled with disabled zlib support.");
#endif //HAVE_ZLIB
    }
    else if (bz2ed) {
#ifdef HAVE_BZLIB
        BZFILE* bz_stream = BZ2_bzopen(path.c_str(), "rb");
        if (!bz_stream) {
            throw RunTimeError("can't open .bz2 input file: " + path);
        }
        bzip2_istreambuf istrbuf(bz_stream);
        istream is(&istrbuf);
        ret = guess_and_load_stream(is, path.substr(0, len-3),
                                    format_name, options);
#else
        throw RunTimeError("Program is compiled with disabled bzlib support.");
#endif //HAVE_BZLIB
    }
    else {
        ifstream is(path.c_str(), ios::in | ios::binary);
        if (!is)
            throw RunTimeError("can't open input file: " + path);
        ret = guess_and_load_stream(is, path, format_name, options);
    }
    return ret;
}


DataSet* load_stream(istream &is, string const& format_name,
                     string const& options)
{
    xylibFormat const* xf = xylib_get_format_by_name(format_name.c_str());
    FormatInfo const* fi = static_cast<FormatInfo const*>(xf);
    return load_stream_of_format(is, fi, options);
}


// filename: path, filename or only extension with dot
vector<FormatInfo const*> get_possible_filetypes(string const& filename)
{
    vector<FormatInfo const*> results;

    // get extension
    string::size_type pos = filename.find_last_of('.');
    string ext = (pos == string::npos) ? string() : filename.substr(pos + 1);

    for (FormatInfo const **i = formats; *i != NULL; ++i) {
        string exts = (*i)->exts;
        if (exts.empty() || (!ext.empty() && has_word(exts, str_tolower(ext))))
            results.push_back(*i);
    }
    return results;
}

FormatInfo const* guess_filetype(const string &path, istream &f)
{
    vector<FormatInfo const*> possible = get_possible_filetypes(path);
    for (vector<FormatInfo const*>::const_iterator i = possible.begin();
                                                i != possible.end(); ++i) {
        if (check_format(*i, f))
            return *i;
        f.seekg(0);
        f.clear();
    }
    return NULL;
}


// all_files is a string used to show all file ("*" or "*.*")
string get_wildcards_string(string const& all_files)
{
    string r;
    for (FormatInfo const **i = formats; *i != NULL; ++i) {
        if (!r.empty())
            r += "|";
        string ext_list;
        const char* exts = (*i)->exts;
        int len = strlen(exts);

        if (len == 0)
            ext_list = all_files;
        else {
            const char* start = exts;
            for (;;) {
                if (start != exts)
                    ext_list += ";";
                const char* end = strchr(start, ' ');
                int ext_len = (end == NULL ? len - (start - exts)
                                           : end - start);
                string ext(start, ext_len);
                ext_list += "*." + ext;
#ifdef HAVE_ZLIB
                ext_list += ";*." + ext + ".gz";
#endif
#ifdef HAVE_BZLIB
                ext_list += ";*." + ext + ".bz2";
#endif
                if (end == NULL)
                    break;
                start = end + 1;
                assert(isalnum(*start));
            }
        }
        string up = ext_list;
        transform(up.begin(), up.end(), up.begin(), (int(*)(int)) toupper);
        r += string((*i)->desc) + " (" + ext_list + ")|" + ext_list;
        if (up != ext_list) // if it contains only (*.*) it won't be appended
            r += ";" + up;
    }
    return r;
}

} // namespace xylib

