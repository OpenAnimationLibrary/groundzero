# SPDX-License-Identifier: BSD-3-Clause
# Safety/portability corrections to the verified Partio PRT implementation.
# The original copyright and license text remains in the generated source.
# Exact replacements deliberately fail when the pinned upstream source changes.
function(ot_patch_partio_prt source output)
    file(READ "${source}" _prt_source)
    macro(_prt_replace old new)
        string(FIND "${_prt_source}" "${old}" _prt_match)
        if(_prt_match LESS 0)
            message(FATAL_ERROR "Pinned Partio PRT source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _prt_source "${_prt_source}")
    endmacro()
    _prt_replace([=[#include <iostream>]=] [=[#include <iostream>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>]=])
    _prt_replace([=[#define AUTO_CASES    // auto upcase ie:position => Position]=] [=[// OT: preserve attribute names exactly; mapping is explicit in the OT adapter.]=])
    _prt_replace([=[#define OUT_BUFSIZE		(4096)]=] [=[#define OUT_BUFSIZE (4096)

// OT: unaligned channel offsets are legal; never dereference a cast pointer.
template<class T> static T prtLoad(const char* bytes) {
    T value; std::memcpy(&value, bytes, sizeof(value)); return value;
}
template<class T> static int prtInteger(const char* bytes) {
    const T value = prtLoad<T>(bytes);
    if constexpr (std::is_signed<T>::value) {
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            throw std::runtime_error("PRT integer exceeds Partio's 32-bit integer representation");
    } else if (value > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("PRT integer exceeds Partio's 32-bit integer representation");
    }
    return static_cast<int>(value);
}
struct PrtInflateGuard {
    z_stream* stream;
    ~PrtInflateGuard() { if (stream) inflateEnd(stream); }
};
struct PrtDeflateGuard {
    z_stream* stream;
    ~PrtDeflateGuard() { if (stream) deflateEnd(stream); }
};]=])
    _prt_replace([=[static bool read_buffer(std::istream& is, z_stream& z, char* in_buf, void* p, size_t size, std::ostream* errorStream) {
    z.next_out=(Bytef*)p;
    z.avail_out=(uInt)size;

    while (z.avail_out) {
        if ( z.avail_in == 0 ) {
            if (!is.eof()) {
                z.next_in = (Bytef*)in_buf;
                is.read((char*)z.next_in, OUT_BUFSIZE);
                if (is.bad()) {
                    if(errorStream) *errorStream<<"read error "<<std::endl;;
                    return false;
                }
                z.avail_in = (uInt)is.gcount();
            }
        }
        int ret = inflate( &z, Z_BLOCK  );
        if ( ret != Z_OK && ret != Z_STREAM_END ) {
            if(errorStream) *errorStream<<"Zlib error "<<z.msg<<std::endl;;
            return false;
        }
        if (ret == Z_STREAM_END && z.avail_out > 0) {
            std::cerr<<"Truncated prt file  "<<std::endl;;
            return false;
        }
    }
    return true;
}

]=] [=[static bool read_buffer(std::istream& is, z_stream& z, char* in_buf, void* p, size_t size, std::ostream* errorStream) {
    if (size > std::numeric_limits<uInt>::max()) return false;
    z.next_out = static_cast<Bytef*>(p);
    z.avail_out = static_cast<uInt>(size);
    while (z.avail_out) {
        if (!z.avail_in) {
            is.read(in_buf, OUT_BUFSIZE);
            const auto got = is.gcount();
            if (got <= 0 || is.bad()) {
                if(errorStream) *errorStream << "Truncated PRT compressed data" << std::endl;
                return false;
            }
            z.next_in = reinterpret_cast<Bytef*>(in_buf);
            z.avail_in = static_cast<uInt>(got);
        }
        const uInt beforeIn = z.avail_in, beforeOut = z.avail_out;
        const int ret = inflate(&z, Z_NO_FLUSH);
        if ((ret != Z_OK && ret != Z_STREAM_END) ||
            (ret == Z_STREAM_END && z.avail_out) ||
            (z.avail_in == beforeIn && z.avail_out == beforeOut)) {
            if(errorStream) *errorStream << "Invalid or truncated PRT compressed data" << std::endl;
            return false;
        }
    }
    return true;
}

]=])
    _prt_replace([=[    FileHeadder header;
    input->read((char*)&header,sizeof(FileHeadder));]=] [=[    std::unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    FileHeadder header{};
    input->read((char*)&header,sizeof(FileHeadder));]=])
    _prt_replace([=[if (memcmp(header.magic, magic, sizeof(magic))) {]=] [=[if (!*input || memcmp(header.magic, magic, sizeof(magic)) ||
        memcmp(header.signature, signature, sizeof(signature)) || header.version != 1 ||
        header.headersize < sizeof(FileHeadder) || header.headersize > 1048576 ||
        header.numParticles > 10000000) {]=])
    _prt_replace([=[    simple->addParticles((int)header.numParticles);]=] [=[    if (!*input || reserve != 4 || channels < 0 || channels > 256 ||
        channelsize < int(sizeof(Channel)) || channelsize > 1048576 ||
        (header.numParticles && channels == 0)) {
        if(errorStream) *errorStream << "Invalid PRT channel table" << std::endl;
        return nullptr;
    }
    simple->addParticles((int)header.numParticles);]=])
    _prt_replace([=[    unsigned particleSize = 0;]=] [=[    unsigned particleSize = 0;
    std::set<std::string> channelNames;]=])
    _prt_replace([=[        Channel ch;
        input->read((char*)&ch, sizeof(Channel));]=] [=[        Channel ch{};
        input->read((char*)&ch, sizeof(Channel));
        if (!*input || !std::memchr(ch.name, 0, sizeof(ch.name)) || !ch.name[0] ||
            ch.type >= 11 || ch.arity == 0 || ch.arity > 4096 ||
            ch.offset > 1073741824u ||
            static_cast<unsigned long long>(ch.offset) + sizes[ch.type] * ch.arity > 1073741824ull) {
            if(errorStream) *errorStream << "Invalid PRT channel name, type, arity or offset" << std::endl;
            return nullptr;
        }
        const unsigned stride = ch.offset + sizes[ch.type] * ch.arity;
        if (header.numParticles && stride > 1073741824ull / header.numParticles) {
            if(errorStream) *errorStream << "PRT decoded data exceeds the memory limit" << std::endl;
            return nullptr;
        }
        if (!channelNames.insert(reinterpret_cast<const char*>(ch.name)).second) {
            if(errorStream) *errorStream << "Duplicate PRT channel name" << std::endl;
            return nullptr;
        }]=])
    _prt_replace([=[particleSize = (std::max)( particleSize, chans.back().offset + sizes[ch.type] );]=] [=[particleSize = (std::max)(particleSize, ch.offset + sizes[ch.type] * ch.arity);]=])
    _prt_replace([=[    if (headersOnly) return simple;]=] [=[    if (!*input) {
        if(errorStream) *errorStream << "Truncated PRT channel table" << std::endl;
        return nullptr;
    }
    if (headersOnly) return owned.release();]=])
    _prt_replace([=[    z_stream z;]=] [=[    z_stream z{};]=])
    _prt_replace([=[    char in_buf[OUT_BUFSIZE];]=] [=[    PrtInflateGuard inflateGuard{&z};
    char in_buf[OUT_BUFSIZE];]=])
    _prt_replace([=[    char* prt_buf = new char[particleSize];]=] [=[    std::vector<char> storage(particleSize);
    char* prt_buf = storage.data();]=])
    _prt_replace([=[        read_buffer(*input, z, (char*)in_buf, prt_buf, particleSize, errorStream);]=] [=[        if (!read_buffer(*input, z, in_buf, prt_buf, particleSize, errorStream)) return nullptr;]=])
    _prt_replace([=[*reinterpret_cast<short*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(short) ] )]=] [=[prtLoad<short>(&prt_buf[ chans[attrIndex].offset + count * sizeof(short) ])]=])
    _prt_replace([=[*reinterpret_cast<int*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(int) ] )]=] [=[prtLoad<int>(&prt_buf[ chans[attrIndex].offset + count * sizeof(int) ])]=])
    _prt_replace([=[*reinterpret_cast<long long*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(long long) ] )]=] [=[prtLoad<long long>(&prt_buf[ chans[attrIndex].offset + count * sizeof(long long) ])]=])
    _prt_replace([=[*reinterpret_cast<unsigned short*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(unsigned short) ] )]=] [=[prtLoad<unsigned short>(&prt_buf[ chans[attrIndex].offset + count * sizeof(unsigned short) ])]=])
    _prt_replace([=[*reinterpret_cast<unsigned int*>( &prt_buf[ chans[attrIndex].offset +  + count * sizeof(unsigned int) ] )]=] [=[prtLoad<unsigned int>(&prt_buf[ chans[attrIndex].offset +  + count * sizeof(unsigned int) ])]=])
    _prt_replace([=[*reinterpret_cast<unsigned long long*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(unsigned long long) ] )]=] [=[prtLoad<unsigned long long>(&prt_buf[ chans[attrIndex].offset + count * sizeof(unsigned long long) ])]=])
    _prt_replace([=[*reinterpret_cast<unsigned char*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(unsigned char) ] )]=] [=[prtLoad<unsigned char>(&prt_buf[ chans[attrIndex].offset + count * sizeof(unsigned char) ])]=])
    _prt_replace([=[*reinterpret_cast<half*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(half) ] )]=] [=[prtLoad<half>(&prt_buf[ chans[attrIndex].offset + count * sizeof(half) ])]=])
    _prt_replace([=[*reinterpret_cast<float*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(float) ] )]=] [=[prtLoad<float>(&prt_buf[ chans[attrIndex].offset + count * sizeof(float) ])]=])
    _prt_replace([=[*reinterpret_cast<double*>( &prt_buf[ chans[attrIndex].offset + count * sizeof(double) ] )]=] [=[prtLoad<double>(&prt_buf[ chans[attrIndex].offset + count * sizeof(double) ])]=])
    _prt_replace([=[(int)prtLoad<short>]=] [=[prtInteger<short>]=])
    _prt_replace([=[(int)prtLoad<int>]=] [=[prtInteger<int>]=])
    _prt_replace([=[(int)prtLoad<long long>]=] [=[prtInteger<long long>]=])
    _prt_replace([=[(int)prtLoad<unsigned short>]=] [=[prtInteger<unsigned short>]=])
    _prt_replace([=[(int)prtLoad<unsigned int>]=] [=[prtInteger<unsigned int>]=])
    _prt_replace([=[(int)prtLoad<unsigned long long>]=] [=[prtInteger<unsigned long long>]=])
    _prt_replace([=[(int)prtLoad<unsigned char>]=] [=[prtInteger<unsigned char>]=])
    _prt_replace([=[    delete[] prt_buf;]=] [=[    // Storage is released by its vector owner.]=])
    _prt_replace([=[    if (inflateEnd( &z ) != Z_OK) {
        if(errorStream) *errorStream<<"Zlib inflateEnd error"<<std::endl;
        return 0;
    }

    // success
    return simple;]=] [=[    // Require a completed zlib stream and no undeclared particle payload.
    unsigned char extra = 0;
    z.next_out = &extra; z.avail_out = 1;
    for (;;) {
        if (!z.avail_in && !input->eof()) {
            input->read(in_buf, OUT_BUFSIZE);
            z.next_in = reinterpret_cast<Bytef*>(in_buf);
            z.avail_in = static_cast<uInt>(input->gcount());
        }
        const uInt before = z.avail_in;
        const int ret = inflate(&z, Z_NO_FLUSH);
        if (!z.avail_out || (ret != Z_OK && ret != Z_STREAM_END) ||
            (ret != Z_STREAM_END && before == z.avail_in)) {
            if(errorStream) *errorStream << "Invalid PRT compressed stream ending" << std::endl;
            return nullptr;
        }
        if (ret == Z_STREAM_END) break;
    }
    return owned.release();]=])
    _prt_replace([=[    if (numParts)
    {]=] [=[    if (numParts >= 0)
    {]=])
    _prt_replace([=[                    memcpy((char*)ch.name, attr.name.c_str(), attr.name.size());]=] [=[            if (attr.name.empty() || attr.name.size() >= sizeof(ch.name) ||
                attr.count < 1 || attr.count > 4096 ||
                (attr.type != FLOAT && attr.type != VECTOR && attr.type != INT)) {
                if(errorStream) *errorStream << "Invalid PRT export channel" << std::endl;
                return false;
            }
            memcpy((char*)ch.name, attr.name.c_str(), attr.name.size());]=])
    _prt_replace([=[        char out_buf[OUT_BUFSIZE+10];]=] [=[        PrtDeflateGuard deflateGuard{&z};
        char out_buf[OUT_BUFSIZE+10];]=])
    _prt_replace([=[        write_buffer(*output, z, (char*)out_buf, 0, 0, true, errorStream);
        if (deflateEnd( &z ) != Z_OK) {]=] [=[        if (!write_buffer(*output, z, out_buf, nullptr, 0, true, errorStream)) return false;
        const int endStatus = deflateEnd(&z);
        deflateGuard.stream = nullptr;
        if (endStatus != Z_OK) {]=])
    _prt_replace([=[        // success
    }// end if numParticles > 0]=] [=[        output->flush();
        if (!*output) {
            if(errorStream) *errorStream << "Failed to finish PRT export" << std::endl;
            return false;
        }
        // success
    }// end if numParticles >= 0]=])
    file(WRITE "${output}" "${_prt_source}")
endfunction()
