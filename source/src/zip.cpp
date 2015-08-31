#include "cube.h"

// zip internals

enum
{
    ZIP_LOCAL_FILE_SIGNATURE = 0x04034B50,
    ZIP_LOCAL_FILE_SIZE      = 30,
    ZIP_FILE_SIGNATURE       = 0x02014B50,
    ZIP_FILE_SIZE            = 46,
    ZIP_DIRECTORY_SIGNATURE  = 0x06054B50,
    ZIP_DIRECTORY_SIZE       = 22
};

struct ziplocalfileheader
{
    uint signature;
    ushort version, flags, compression, modtime, moddate;
    uint crc32, compressedsize, uncompressedsize;
    ushort namelength, extralength;
};

struct zipfileheader
{
    uint signature;
    ushort version, needversion, flags, compression, modtime, moddate;
    uint crc32, compressedsize, uncompressedsize;
    ushort namelength, extralength, commentlength, disknumber, internalattribs;
    uint externalattribs, offset;
};

struct zipdirectoryheader
{
    uint signature;
    ushort disknumber, directorydisk, diskentries, entries;
    uint size, offset;
    ushort commentlength;
};

// manage zips

struct zipstream;
struct ziparchive;

struct zipfile // file in a zip archive
{
    char *name, *fullname; // name from zip, full path name
    uint header, offset, size, compressedsize;
    ziparchive *location;

    zipfile() : name(NULL), fullname(NULL), header(0), offset(~0U), size(0), compressedsize(0), location(NULL) { }
    ~zipfile() { DELSTRING(name); DELSTRING(fullname); }
};

struct ziparchive // zip archive file
{
    char *name;
    stream *data;
    vector<zipfile> files;
    int openfiles;
    zipstream *owner;

    ziparchive() : name(NULL), data(NULL), openfiles(0), owner(NULL) { }
    ~ziparchive() { DELSTRING(name); DELETEP(data); }
};

static bool findzipdirectory(stream *f, zipdirectoryheader &hdr)
{
    if(!f->seek(0, SEEK_END)) return false;

    uchar buf[1024], *src = NULL;
    int len = 0, offset = f->tell(), end = max(offset - 0xFFFF - ZIP_DIRECTORY_SIZE, 0);
    const uint signature = lilswap<uint>(ZIP_DIRECTORY_SIGNATURE);

    while(offset > end)
    {
        int carry = min(len, ZIP_DIRECTORY_SIZE-1), next = min((int)sizeof(buf) - carry, offset - end);
        offset -= next;
        memmove(&buf[next], buf, carry);
        if(next + carry < ZIP_DIRECTORY_SIZE || !f->seek(offset, SEEK_SET) || (int)f->read(buf, next) != next) return false;
        len = next + carry;
        uchar *search = &buf[next-1];
        for(; search >= buf; search--) if(*(uint *)search == signature) break;
        if(search >= buf) { src = search; break; }
    }

    if(!src || &buf[len] - src < ZIP_DIRECTORY_SIZE) return false;

    hdr.signature = lilswap(*(uint *)src); src += 4;
    hdr.disknumber = lilswap(*(ushort *)src); src += 2;
    hdr.directorydisk = lilswap(*(ushort *)src); src += 2;
    hdr.diskentries = lilswap(*(ushort *)src); src += 2;
    hdr.entries = lilswap(*(ushort *)src); src += 2;
    hdr.size = lilswap(*(uint *)src); src += 4;
    hdr.offset = lilswap(*(uint *)src); src += 4;
    hdr.commentlength = lilswap(*(ushort *)src); src += 2;

    if(hdr.signature != ZIP_DIRECTORY_SIGNATURE || hdr.disknumber != hdr.directorydisk || hdr.diskentries != hdr.entries) return false;

    return true;
}

#ifndef STANDALONE
VAR(dbgzip, 0, 0, 1);
#endif

static bool readzipdirectory(stream *f, int entries, int offset, int size, vector<zipfile> &files)
{
    uchar *buf = new uchar[size], *src = buf;
    if(!f->seek(offset, SEEK_SET) || (int)f->read(buf, size) != size) { delete[] buf; return false; }
    loopi(entries)
    {
        if(src + ZIP_FILE_SIZE > &buf[size]) break;

        zipfileheader hdr;
        hdr.signature = lilswap(*(uint *)src); src += 4;
        hdr.version = lilswap(*(ushort *)src); src += 2;
        hdr.needversion = lilswap(*(ushort *)src); src += 2;
        hdr.flags = lilswap(*(ushort *)src); src += 2;
        hdr.compression = lilswap(*(ushort *)src); src += 2;
        hdr.modtime = lilswap(*(ushort *)src); src += 2;
        hdr.moddate = lilswap(*(ushort *)src); src += 2;
        hdr.crc32 = lilswap(*(uint *)src); src += 4;
        hdr.compressedsize = lilswap(*(uint *)src); src += 4;
        hdr.uncompressedsize = lilswap(*(uint *)src); src += 4;
        hdr.namelength = lilswap(*(ushort *)src); src += 2;
        hdr.extralength = lilswap(*(ushort *)src); src += 2;
        hdr.commentlength = lilswap(*(ushort *)src); src += 2;
        hdr.disknumber = lilswap(*(ushort *)src); src += 2;
        hdr.internalattribs = lilswap(*(ushort *)src); src += 2;
        hdr.externalattribs = lilswap(*(uint *)src); src += 4;
        hdr.offset = lilswap(*(uint *)src); src += 4;
        if(hdr.signature != ZIP_FILE_SIGNATURE) break;
        if(!hdr.namelength /*|| !hdr.uncompressedsize */|| (hdr.compression && (hdr.compression != Z_DEFLATED || !hdr.compressedsize)))
        {
            src += hdr.namelength + hdr.extralength + hdr.commentlength;
            continue;
        }
        if(src + hdr.namelength > &buf[size]) break;

        string pname;
        int namelen = min((int)hdr.namelength, (int)sizeof(pname)-1);
        memcpy(pname, src, namelen);
        pname[namelen] = '\0';
        path(pname);
        char *name = newstring(pname);

        zipfile &zf = files.add();
        zf.name = name;
        zf.header = hdr.offset;
        zf.size = hdr.uncompressedsize;
        zf.compressedsize = hdr.compression ? hdr.compressedsize : 0;
#ifndef STANDALONE
        if(dbgzip) conoutf("file %s, size %d, compress %d, flags %x", name, hdr.uncompressedsize, hdr.compression, hdr.flags);
#endif

        src += hdr.namelength + hdr.extralength + hdr.commentlength;
    }
    delete[] buf;

    return files.length() > 0;
}

static bool readlocalfileheader(stream *f, ziplocalfileheader &h, uint offset)
{
    f->seek(offset, SEEK_SET);
    uchar buf[ZIP_LOCAL_FILE_SIZE];
    if(f->read(buf, ZIP_LOCAL_FILE_SIZE) != ZIP_LOCAL_FILE_SIZE)
        return false;
    uchar *src = buf;
    h.signature = lilswap(*(uint *)src); src += 4;
    h.version = lilswap(*(ushort *)src); src += 2;
    h.flags = lilswap(*(ushort *)src); src += 2;
    h.compression = lilswap(*(ushort *)src); src += 2;
    h.modtime = lilswap(*(ushort *)src); src += 2;
    h.moddate = lilswap(*(ushort *)src); src += 2;
    h.crc32 = lilswap(*(uint *)src); src += 4;
    h.compressedsize = lilswap(*(uint *)src); src += 4;
    h.uncompressedsize = lilswap(*(uint *)src); src += 4;
    h.namelength = lilswap(*(ushort *)src); src += 2;
    h.extralength = lilswap(*(ushort *)src); src += 2;
    if(h.signature != ZIP_LOCAL_FILE_SIGNATURE) return false;
    // h.uncompressedsize or h.compressedsize may be zero - so don't validate
    return true;
}

#if 0
ziparchive *findzip(const char *name)
{
    loopv(archives) if(!strcmp(name, archives[i]->name)) return archives[i];
    return NULL;
}

static bool checkprefix(vector<zipfile> &files, const char *prefix, int prefixlen)
{
    loopv(files)
    {
        if(!strncmp(files[i].name, prefix, prefixlen)) return false;
    }
    return true;
}

bool extractzipfile(ziparchive *a, zipfile *f, const char *name);

bool fitspackage(char *filename, int type)
{
    if(strchr(filename, ' ')) return false; // refuse all filenames with spaces
    char *extension = strrchr(filename, '.');
    if(!extension) return false;
    ++extension;
    switch(type)
    {
        case PCK_MAPMODEL:  return !strcmp(extension, "md2") || !strcmp(extension, "md3")
                               || !strcmp(extension, "cfg") || !strcmp(extension, "txt")
                               || !strcmp(extension, "jpg") || !strcmp(extension, "png");
        case PCK_MAP:       return !strcmp(extension, "cgz") || !strcmp(extension, "cfg");
        case PCK_SKYBOX:    return !strcmp(extension, "jpg") || !strcmp(extension, "png");
        default:            return true;
    }
}

static void mountzip(ziparchive &arch, vector<zipfile> &files, const char *mountdir, const char *stripdir, bool extract)
{
    string packagesdir = "packages/";
    path(packagesdir);
    int striplen = stripdir ? (int)strlen(stripdir) : 0;
    if(arch.type == PCK_MAP) mountdir = "packages/maps/";
    if(!mountdir && !stripdir) loopv(files)
    {
        zipfile &f = files[i];
        const char *foundpackages = strstr(f.name, packagesdir);
        if(foundpackages)
        {
            if(foundpackages > f.name)
            {
                stripdir = f.name;
                striplen = foundpackages - f.name;
            }
            break;
        }
        const char *foundcgz = strstr(f.name, ".cgz");
        if(foundcgz)
        {
            const char *cgzdir = foundcgz;
            while(--cgzdir >= f.name && *cgzdir != PATHDIV);
            if(cgzdir < f.name || checkprefix(files, f.name, cgzdir + 1 - f.name))
            {
                if(cgzdir >= f.name)
                {
                    stripdir = f.name;
                    striplen = cgzdir + 1 - f.name;
                }
                if(!mountdir) mountdir = "packages/maps/";
                break;
            }
            mountdir = "packages/maps/";
        }
    }
    string mdir = "", fname;
    if(mountdir)
    {
        copystring(mdir, mountdir);
        if(fixpackagedir(mdir) <= 1) mdir[0] = '\0';
    }
    loopv(files)
    {
        zipfile &f = files[i];
        formatstring(fname)("%s%s", mdir, striplen && !strncmp(f.name, stripdir, striplen) ? &f.name[striplen] : f.name);
        if(arch.files.access(fname)) continue;
        char *mname = newstring(fname);
        zipfile &mf = arch.files[mname];
        mf = f;
        mf.name = mname;

        if(extract)
        {
            if(fitspackage(fname, arch.type)) extractzipfile(&arch, &f, fname);
            else conoutf("\f3could not extract \"%s\" : file extension not supported", fname);
        }
    }
}

bool addzip(const char *name, const char *mount, const char *strip, bool extract, int type)
{
    string pname;
    copystring(pname, name);
    path(pname);
    /*int plen = (int)strlen(pname);
    if(plen < 4 || !strchr(&pname[plen-4], '.')) concatstring(pname, ".zip");*/

    /*ziparchive *exists = findzip(pname);
    if(exists)
    {
        conoutf("already added zip %s", pname);
        return true;
    }*/

    stream *f = openfile(pname, "rb");
    if(!f)
    {
        conoutf("could not open file %s", pname);
        return false;
    }
    zipdirectoryheader h;
    vector<zipfile> files;
    if(!findzipdirectory(f, h) || !readzipdirectory(f, h.entries, h.offset, h.size, files))
    {
        conoutf("could not read directory in zip %s", pname);
        delete f;
        return false;
    }

    ziparchive *arch = new ziparchive;
    arch->name = newstring(pname);
    arch->data = f;
    arch->type = type;
    mountzip(*arch, files, mount, strip, extract);
    if(!extract) archives.add(arch);
    else delete arch;
    return true;
}

bool removezip(const char *name)
{
    string pname;
    copystring(pname, name);
    path(pname);
    /*int plen = (int)strlen(pname);
    if(plen < 4 || !strchr(&pname[plen-4], '.')) concatstring(pname, ".zip");*/
    ziparchive *exists = findzip(pname);
    if(!exists)
    {
        conoutf("zip %s is not loaded", pname);
        return false;
    }
    if(exists->openfiles)
    {
        conoutf("zip %s has open files", pname);
        return false;
    }
    //conoutf("removed zip %s", exists->name);
    archives.removeobj(exists);
    delete exists;
    return true;
}
#endif

// stream interface

struct zipstream : stream
{
    enum
    {
        BUFSIZE  = 16384
    };

    ziparchive *arch;
    zipfile *info;
    z_stream zfile;
    uchar *buf;
    int reading;
    bool ended;

    zipstream() : arch(NULL), info(NULL), buf(NULL), reading(-1), ended(false)
    {
        zfile.zalloc = NULL;
        zfile.zfree = NULL;
        zfile.opaque = NULL;
        zfile.next_in = zfile.next_out = NULL;
        zfile.avail_in = zfile.avail_out = 0;
    }

    ~zipstream()
    {
        close();
    }

    void readbuf(uint size = BUFSIZE)
    {
        if(!zfile.avail_in) zfile.next_in = (Bytef *)buf;
        size = min(size, uint(&buf[BUFSIZE] - &zfile.next_in[zfile.avail_in]));
        if(arch->owner != this)
        {
            arch->owner = NULL;
            if(arch->data->seek(reading, SEEK_SET)) arch->owner = this;
            else return;
        }
        uint remaining = info->offset + info->compressedsize - reading;
        int n = arch->owner == this ? (int)arch->data->read(zfile.next_in + zfile.avail_in, min(size, remaining)) : 0;
        zfile.avail_in += n;
        reading += n;
    }

    bool open(ziparchive *a, zipfile *zf)
    {
        if(zf->offset == ~0U)
        {
            ziplocalfileheader h;
            a->owner = NULL;
            if(!readlocalfileheader(a->data, h, zf->header)) return false;
            zf->offset = zf->header + ZIP_LOCAL_FILE_SIZE + h.namelength + h.extralength;
        }

        if(zf->compressedsize && inflateInit2(&zfile, -MAX_WBITS) != Z_OK) return false;

        a->openfiles++;
        arch = a;
        info = zf;
        reading = zf->offset;
        ended = false;
        if(zf->compressedsize) buf = new uchar[BUFSIZE];
        return true;
    }

    void stopreading()
    {
        if(reading < 0) return;
#ifndef STANDALONE
        if(dbgzip) conoutf(info->compressedsize ? "%s: zfile.total_out %d, info->size %u" : "%s: reading %d, info->size %u", info->name, int(info->compressedsize ? zfile.total_out : reading - info->offset), info->size);
#endif
        if(info->compressedsize) inflateEnd(&zfile);
        reading = -1;
    }

    void close()
    {
        stopreading();
        DELETEA(buf);
        if(arch) { arch->owner = NULL; arch->openfiles--; arch = NULL; }
    }

    long size() { return info->size; }
    bool end() { return reading < 0 || ended; }
    long tell() { return reading >= 0 ? (info->compressedsize ? zfile.total_out : reading - info->offset) : -1; }

    bool seek(long pos, int whence)
    {
        if(reading < 0) return false;
        if(!info->compressedsize)
        {
            switch(whence)
            {
                case SEEK_END: pos += info->offset + info->size; break;
                case SEEK_CUR: pos += reading; break;
                case SEEK_SET: pos += info->offset; break;
                default: return false;
            }
            pos = clamp(pos, long(info->offset), long(info->offset + info->size));
            arch->owner = NULL;
            if(!arch->data->seek(pos, SEEK_SET)) return false;
            arch->owner = this;
            reading = pos;
            ended = false;
            return true;
        }

        switch(whence)
        {
            case SEEK_END: pos += info->size; break;
            case SEEK_CUR: pos += zfile.total_out; break;
            case SEEK_SET: break;
            default: return false;
        }

        if(pos >= (long)info->size)
        {
            reading = info->offset + info->compressedsize;
            zfile.next_in += zfile.avail_in;
            zfile.avail_in = 0;
            zfile.total_in = info->compressedsize;
            arch->owner = NULL;
            ended = false;
            return true;
        }

        if(pos < 0) return false;
        if(pos >= (long)zfile.total_out) pos -= zfile.total_out;
        else
        {
            if(zfile.next_in && zfile.total_in <= uint(zfile.next_in - buf))
            {
                zfile.avail_in += zfile.total_in;
                zfile.next_in -= zfile.total_in;
            }
            else
            {
                arch->owner = NULL;
                zfile.avail_in = 0;
                zfile.next_in = NULL;
                reading = info->offset;
            }
            inflateReset(&zfile);
        }

        uchar skip[512];
        while(pos > 0)
        {
            int skipped = min(pos, (long)sizeof(skip));
            if(read(skip, skipped) != skipped) return false;
            pos -= skipped;
        }

        ended = false;
        return true;
    }

    int read(void *buf, int len)
    {
        if(reading < 0 || !buf || !len) return 0;
        if(!info->compressedsize)
        {
            if(arch->owner != this)
            {
                arch->owner = NULL;
                if(!arch->data->seek(reading, SEEK_SET)) { stopreading(); return 0; }
                arch->owner = this;
            }

            int n = (int)arch->data->read(buf, min(len, int(info->size + info->offset - reading)));
            reading += n;
            if(n < len) ended = true;
            return n;
        }

        zfile.next_out = (Bytef *)buf;
        zfile.avail_out = len;
        while(zfile.avail_out > 0)
        {
            if(!zfile.avail_in) readbuf(BUFSIZE);
            int err = inflate(&zfile, Z_NO_FLUSH);
            if(err != Z_OK)
            {
                if(err == Z_STREAM_END) ended = true;
                else
                {
#ifndef STANDALONE
                    if(dbgzip) conoutf("inflate error: %s", zError(err));
#endif
                    stopreading();
                }
                break;
            }
        }
        return len - zfile.avail_out;
    }
};

// manage zips as part of the AC virtual filesystem

static vector<ziparchive *> archives; // list of mounted zip files, highest priority first
static hashtable<const char *, zipfile *> zipfiles; // table of all files in all mounted zips

static void mountzip(ziparchive &arch, const char *mountdir, const char *stripdir, bool allowconfig)
{
    string mdir = "", fname;
    if(mountdir)
    {
        copystring(mdir, mountdir);
        if(fixpackagedir(mdir) <= 1) mdir[0] = '\0';
    }
    int striplen = stripdir ? (int)strlen(stripdir) : 0;

    loopv(arch.files)
    { // build full path names - and check them
        zipfile *zf = &arch.files[i];
        formatstring(fname)("%s%s", mdir, striplen && !strncmp(zf->name, stripdir, striplen) ? zf->name + striplen : zf->name);
        path(fname);
        unixpath(fname);
        zf->fullname = newstring(fname);
        zf->location = &arch;
        filtertext(fname, fname, FTXT__MEDIAFILEPATH);
        if(strcmp(fname, zf->fullname) || (strncmp("packages/", fname, 9) && (!allowconfig || strncmp("config/", fname, 7))))
        { // path is not allowed (illegal characters or illegal path)
#if defined(_DEBUG) && !defined(STANDALONE)
            clientlogf("mountzip: ignored file %s from zip %s", fname, arch.name);
#endif
            arch.files.remove(i--).~zipfile();
        }
        else path(zf->fullname);
    }
}

static void rebuildzipfilehashtable()
{
    zipfiles.clear(false);
    loopv(archives)
    {
        ziparchive *arch = archives[i];
        loopvj(arch->files)
        {
            zipfile *zf = &arch->files[j];
            zipfiles.access(zf->fullname, zf);
        }
    }
}

// manage zipped mod packages in subdirectory "mods/" including description and preview images
// zip packages are restricted to contain files below "packages/" only - unless the zip filename starts with "###", which additionally allows files below "config/"

ziparchive *findzip(const char *name)
{
    loopv(archives) if(!strcmp(name, archives[i]->name)) return archives[i];
    return NULL;
}

#ifndef STANDALONE

const char *fixzipname(char *name) // check, if valid path + filename, based on mods/, return NULL if invalid
{
    string tmp;
    unixpath(name); // changes "name"!
    filtertext(tmp, name, FTXT__MEDIAFILEPATH);
    const char *bp = behindpath(tmp), *pd = parentdir(tmp);
    static defformatstring(pname)("%s%s%s%s", pd, bp != tmp ? "/" : "", !strncmp("###", behindpath(name), 3) ? "###" : "", bp);
    if(strlen(name) > 40 || strchr(name, '.') || strcmp(pname, name) || *name == '/') return NULL; // illegal filename
    formatstring(pname)("mods/%s.zip", name);
    return path(pname);
}

VAR(zipcachemaxsize, 0, 512, 1024); // load zip files smaller than 512K straight into RAM

void addzipmod(char *name)
{
    const char *pname = fixzipname(name);
    if(!pname)
    {
        conoutf("addzipmod: illegal filename %s", name);
        return;
    }
    ziparchive *exists = findzip(name);
    if(exists)
    { // already added zip
        archives.removeobj(exists);
        archives.add(exists); // sort to the end of the list
        rebuildzipfilehashtable();
        return;
    }
    ziparchive *arch = new ziparchive;
    arch->name = newstring(name);
    if(!(arch->data = openfile(pname, "rb")))
    {
        conoutf("could not open file %s", pname);
        delete arch;
        return;
    }
    int zipsize = arch->data->size();
    if(zipsize > 0 && zipsize < zipcachemaxsize * 1024)
    {
        uchar *buf = new uchar[zipsize];
        if(arch->data->read(buf, zipsize) != zipsize)
        {
            conoutf("could not cache file %s", pname);
            delete[] buf;
            delete arch;
            return;
        }
        delete arch->data;
        arch->data = openmemfile(buf, zipsize, NULL);
    }
    zipdirectoryheader h;
    if(!findzipdirectory(arch->data, h) || !readzipdirectory(arch->data, h.entries, h.offset, h.size, arch->files))
    {
        conoutf("could not read directory in zip %s", pname);
        delete arch;
        return;
    }
    mountzip(*arch, NULL, NULL, !strncmp(behindpath(name), "###", 3));
    archives.add(arch);
    rebuildzipfilehashtable();
    clientlogf("added zipmod %s, %d bytes, %d files", pname, zipsize, arch->files.length());
}
COMMAND(addzipmod, "s");

void writezipmodconfig(stream *f)
{
    f->printf("zipcachemaxsize %d\n", zipcachemaxsize);
    loopv(archives) f->printf("addzipmod %s\n", archives[i]->name);
}

#endif

// AC virtual filesystem

bool findzipfile(const char *name)
{
    return zipfiles.access(name) != NULL;
}

stream *openzipfile(const char *name, const char *mode)
{
    for(; *mode; mode++) if(*mode=='w' || *mode=='a') return NULL;
//    if(!strncmp(name, "zip://", 6)) name += 6;
    zipfile **zf = zipfiles.access(name);
    if(zf && *zf)
    {
        zipstream *s = new zipstream;
        if(s->open((*zf)->location, *zf)) return s;
        delete s;
    }
    return NULL;
}

void listzipfiles(const char *dir, const char *ext, vector<char *> &files) // (does not list memfiles)
{
    int extsize = ext ? (int)strlen(ext)+1 : 0, dirsize = (int)strlen(dir);
    enumerate(zipfiles, zipfile *, zf,
    {
        if(strncmp(zf->fullname, dir, dirsize)) continue;
        const char *name = zf->fullname + dirsize;
        if(name[0] == PATHDIV) name++;
        if(strchr(name, PATHDIV)) continue;
        if(!ext) files.add(newstring(name));
        else
        {
            int namelength = (int)strlen(name) - extsize;
            if(namelength > 0 && name[namelength] == '.' && strncmp(name+namelength+1, ext, extsize-1)==0)
                files.add(newstring(name, namelength));
        }
    });
}


