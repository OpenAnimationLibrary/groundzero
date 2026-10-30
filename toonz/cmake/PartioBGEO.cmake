# SPDX-License-Identifier: BSD-3-Clause
# Strict, bounded replacement for the pinned Partio classic BGEO reader.
# The generated source retains the upstream copyright, license and writer.
# The replaced reader span is identified by its SHA-256, so the patch fails
# when the reviewed upstream implementation changes.
function(ot_patch_partio_bgeo source output)
    file(READ "${source}" _bgeo_source)
    function(_bgeo_replace old new)
        string(FIND "${_bgeo_source}" "${old}" _bgeo_match)
        if(_bgeo_match LESS 0)
            message(FATAL_ERROR "Pinned Partio BGEO source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _bgeo_source "${_bgeo_source}")
        set(_bgeo_source "${_bgeo_source}" PARENT_SCOPE)
    endfunction()
    # Replace the text from `first` through the end of `last` after verifying it.
    function(_bgeo_rewrite first last digest new)
        string(FIND "${_bgeo_source}" "${first}" _bgeo_begin)
        string(FIND "${_bgeo_source}" "${last}" _bgeo_end)
        if(_bgeo_begin LESS 0 OR _bgeo_end LESS _bgeo_begin)
            message(FATAL_ERROR "Pinned Partio BGEO source no longer matches its reviewed patch")
        endif()
        string(LENGTH "${last}" _bgeo_last)
        math(EXPR _bgeo_length "${_bgeo_end} + ${_bgeo_last} - ${_bgeo_begin}")
        string(SUBSTRING "${_bgeo_source}" ${_bgeo_begin} ${_bgeo_length} _bgeo_span)
        string(SHA256 _bgeo_digest "${_bgeo_span}")
        if(NOT _bgeo_digest STREQUAL digest)
            message(FATAL_ERROR "Pinned Partio BGEO reader no longer matches its reviewed patch")
        endif()
        string(SUBSTRING "${_bgeo_source}" 0 ${_bgeo_begin} _bgeo_head)
        math(EXPR _bgeo_tail_start "${_bgeo_begin} + ${_bgeo_length}")
        string(SUBSTRING "${_bgeo_source}" ${_bgeo_tail_start} -1 _bgeo_tail)
        set(_bgeo_source "${_bgeo_head}${new}${_bgeo_tail}" PARENT_SCOPE)
    endfunction()
    _bgeo_replace([=[#include "io.h"]=] [=[#include "io.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>]=])
    # Upstream released the particle object inside getAttributes and returned
    # it anyway (use after free), released a null object for primitive
    # attributes, accepted duplicate names (attribute index -1), skipped a
    # negative point count as ~2^64 bytes and loaded truncated point data.
    _bgeo_rewrite([=[template<class TAttribute, class TAccessor>
bool getAttributes(]=] [=[    // return the populated simpleParticle
    return simple;
}
]=] af69e4544baec2d74aec328b33bcd642155839aba557f41a6e25e013364baca3 [=[namespace {
const int bgeoMaxParticles = 10000000;
const int bgeoMaxAttributes = 256;
const int bgeoMaxComponents = 4096;
const std::uint64_t bgeoMaxBytes = 1073741824u;

bool bgeoFail(std::ostream* errorStream, const char* message)
{
    if(errorStream) *errorStream << "Partio: " << message << endl;
    return false;
}

// Forward-only skip (gzip input cannot seek) that requires every byte.
bool bgeoSkip(istream& input, std::uint64_t bytes)
{
    while(bytes){
        const std::uint64_t chunk = (std::min)(bytes, std::uint64_t(1) << 20);
        input.ignore(static_cast<streamsize>(chunk));
        if(!input || static_cast<std::uint64_t>(input.gcount()) != chunk) return false;
        bytes -= chunk;
    }
    return true;
}

// A length-prefixed Houdini string; embedded NUL bytes would alias names.
bool bgeoString(istream& input, string& value)
{
    unsigned short length = 0;
    read<BIGEND>(input, length);
    if(!input) return false;
    value.assign(length, '\0');
    if(length) input.read(&value[0], length);
    return input && input.gcount() == length && value.find('\0') == string::npos;
}

enum class BgeoScope { Point, Primitive, Detail };

// Reads nAttrib attribute definitions. Point and detail attributes are added
// to simple; primitive attributes are only measured, since they are skipped.
// recordSize grows by each attribute's 32-bit component count.
bool bgeoAttributes(istream& input, int nAttrib, BgeoScope scope, ParticlesDataMutable* simple,
                    bool headersOnly, int& recordSize, vector<ParticleAttribute>& points,
                    vector<FixedAttribute>& details, vector<int>& offsets, std::ostream* errorStream)
{
    std::set<string> primitiveNames;
    for(int i = 0; i < nAttrib; i++){
        string name;
        unsigned short size = 0;
        int houdiniType = -1;
        if(!bgeoString(input, name) || name.empty())
            return bgeoFail(errorStream, "invalid or truncated BGEO attribute name");
        read<BIGEND>(input, size, houdiniType);
        if(!input) return bgeoFail(errorStream, "truncated BGEO attribute definition");
        if(size < 1 || size > bgeoMaxComponents || recordSize > int(bgeoMaxBytes / 4) - size)
            return bgeoFail(errorStream, "invalid BGEO attribute size");
        bool duplicate = false;
        if(scope == BgeoScope::Point){
            ParticleAttribute existing;
            duplicate = simple->attributeInfo(name.c_str(), existing);
        }else if(scope == BgeoScope::Detail){
            FixedAttribute existing;
            duplicate = simple->fixedAttributeInfo(name.c_str(), existing);
        }else{
            duplicate = !primitiveNames.insert(name).second;
        }
        if(duplicate) return bgeoFail(errorStream, "duplicate BGEO attribute name");

        ParticleAttributeType type = NONE;
        if(houdiniType == 0) type = FLOAT;
        else if(houdiniType == 1) type = INT;
        else if(houdiniType == 5) type = VECTOR;
        else if(houdiniType == 4) type = INDEXEDSTR;
        else if(houdiniType == 2) return bgeoFail(errorStream, "BGEO attributes of type 'string' are not supported");
        else return bgeoFail(errorStream, "unknown BGEO attribute type");

        ParticleAttribute point;
        FixedAttribute detail;
        if(scope == BgeoScope::Point){
            point = simple->addAttribute(name.c_str(), type, size);
            if(point.attributeIndex < 0) return bgeoFail(errorStream, "cannot add BGEO attribute");
            points.push_back(point);
        }else if(scope == BgeoScope::Detail){
            detail = simple->addFixedAttribute(name.c_str(), type, size);
            if(detail.attributeIndex < 0) return bgeoFail(errorStream, "cannot add BGEO attribute");
            details.push_back(detail);
        }
        offsets.push_back(recordSize);
        recordSize += size;

        if(type != INDEXEDSTR){
            // Default values are not used.
            if(!bgeoSkip(input, std::uint64_t(size) * sizeof(int)))
                return bgeoFail(errorStream, "truncated BGEO attribute defaults");
            continue;
        }
        int numIndices = -1;
        read<BIGEND>(input, numIndices);
        if(!input || numIndices < 0 || numIndices > bgeoMaxParticles)
            return bgeoFail(errorStream, "invalid or truncated BGEO string table");
        std::uint64_t tableBytes = 0;
        for(int ii = 0; ii < numIndices; ii++){
            string value;
            if(!bgeoString(input, value))
                return bgeoFail(errorStream, "invalid or truncated BGEO string table entry");
            tableBytes += value.size() + 1;
            if(tableBytes > bgeoMaxBytes) return bgeoFail(errorStream, "BGEO string table exceeds the memory limit");
            if(headersOnly || scope == BgeoScope::Primitive) continue;
            const int id = scope == BgeoScope::Point
                ? simple->registerIndexedStr(point, value.c_str())
                : simple->registerFixedIndexedStr(detail, value.c_str());
            if(id != ii) return bgeoFail(errorStream, "duplicate BGEO string table entry");
        }
    }
    return true;
}

// Copies one big-endian 32-bit record into its attributes.
void bgeoCopy(const vector<int>& record, const vector<int>& offsets, int first, int count, int* data)
{
    for(int k = 0; k < count; k++){
        int value = record[offsets[first] + k];
        BIGEND::swap(value);
        data[k] = value;
    }
}
}  // namespace

// OT: all-or-nothing classic BGEO reader. Every count, size and read is checked
// before it is used, and the particle object is released exactly once.
ParticlesDataMutable* readBGEO(const char* filename,const bool headersOnly,std::ostream* errorStream)
{
    unique_ptr<istream> input(io::unzip(filename));
    if(!input || !*input){
        if(errorStream) *errorStream<<"Partio: Unable to open file "<<filename<<endl;
        return 0;
    }

    // header values
    char magic[5] = {0, 0, 0, 0, 0};
    char versionChar = 0;
    int version = 0, nPoints = -1, nPrims = -1, nPointGroups = -1, nPrimGroups = -1;
    int nPointAttrib = -1, nVertexAttrib = -1, nPrimAttrib = -1, nAttrib = -1;
    read<BIGEND>(*input,magic[0],magic[1],magic[2],magic[3]);
    read<BIGEND>(*input,versionChar,version,nPoints,nPrims,nPointGroups);
    read<BIGEND>(*input,nPrimGroups,nPointAttrib,nVertexAttrib,nPrimAttrib,nAttrib);
    if(!*input){
        bgeoFail(errorStream, "truncated BGEO header");
        return 0;
    }

    // Check header magic and version
    const char bgeo_magic[5]={'B','g','e','o',0};
    if(strcmp(magic,bgeo_magic)){
        const char new_bgeo_magic[5]={0x7f,0x4e,0x53,0x4a,0};
        if(!strcmp(magic,new_bgeo_magic)){
            if(errorStream) *errorStream<<"Partio: Attempting to read new BGEO format, we only support old BGEO format. Try writing .bhclassic from Houdini."<<std::endl;
        }else{
            if(errorStream) *errorStream<<"Partio: Magic number of '"<<filename<<"' doesn't match bgeo magic '"<<bgeo_magic<<"'"<<endl;
        }
        return 0;
    }
    if(version!=5){
        if(errorStream) *errorStream<<"Partio: BGEO must be version 5"<<endl;
        return 0;
    }
    if(nPoints < 0 || nPoints > bgeoMaxParticles || nPrims < 0 || nPrims > bgeoMaxParticles ||
       nPointGroups < 0 || nPrimGroups < 0 || nVertexAttrib < 0 ||
       nPointAttrib < 0 || nPointAttrib >= bgeoMaxAttributes ||
       nPrimAttrib < 0 || nPrimAttrib > bgeoMaxAttributes ||
       nAttrib < 0 || nAttrib > bgeoMaxAttributes){
        bgeoFail(errorStream, "invalid BGEO point, primitive or attribute count");
        return 0;
    }

    ParticlesDataMutable* simple = headersOnly ? new ParticleHeaders : create();
    unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });

    // Position always occupies the first four record components (x, y, z, w).
    int particleSize = 4;
    vector<int> attrOffsets(1, 0);
    vector<ParticleAttribute> attrHandles(1, simple->addAttribute("position", VECTOR, 3));
    vector<FixedAttribute> unusedFixed;
    if(!bgeoAttributes(*input, nPointAttrib, BgeoScope::Point, simple, headersOnly, particleSize,
                       attrHandles, unusedFixed, attrOffsets, errorStream))
        return 0;
    const std::uint64_t pointBytes = std::uint64_t(nPoints) * std::uint64_t(particleSize) * sizeof(int);
    if(pointBytes > bgeoMaxBytes){
        bgeoFail(errorStream, "BGEO point data exceeds the memory limit");
        return 0;
    }
    simple->addParticles(nPoints);

    if(headersOnly){
        if(!bgeoSkip(*input, pointBytes)){
            bgeoFail(errorStream, "truncated BGEO point data");
            return 0;
        }
    }else{
        vector<int> buffer(particleSize);
        vector<ParticleAccessor> accessors;
        for(size_t i = 0; i < attrHandles.size(); i++) accessors.push_back(ParticleAccessor(attrHandles[i]));
        ParticlesDataMutable::iterator iterator=simple->begin();
        for(size_t i=0;i<accessors.size();i++) iterator.addAccessor(accessors[i]);
        for(ParticlesDataMutable::iterator end=simple->end();iterator!=end;++iterator){
            const streamsize bytes = static_cast<streamsize>(buffer.size() * sizeof(int));
            input->read(reinterpret_cast<char*>(buffer.data()), bytes);
            if(!*input || input->gcount() != bytes){
                bgeoFail(errorStream, "truncated BGEO point data");
                return 0;
            }
            for(size_t attrIndex = 0; attrIndex < attrHandles.size(); attrIndex++)
                bgeoCopy(buffer, attrOffsets, int(attrIndex), attrHandles[attrIndex].count,
                         accessors[attrIndex].raw<int>(iterator));
        }
    }

    // Primitive attributes and particle-system primitives are skipped.
    int primSize = 0;
    vector<int> primOffsets;
    vector<ParticleAttribute> unusedPoints;
    if(!bgeoAttributes(*input, nPrimAttrib, BgeoScope::Primitive, nullptr, true, primSize,
                       unusedPoints, unusedFixed, primOffsets, errorStream))
        return 0;
    for(int i = 0; i < nPrims; i++){
        int primType = 0, size = -1;
        read<BIGEND>(*input, primType);
        if(!*input){
            bgeoFail(errorStream, "truncated BGEO primitive");
            return 0;
        }
        if(primType != 0x00008000){
            // As upstream: keep the points, but detail attributes are not reached.
            if(errorStream) *errorStream << "Partio: Unrecognized Primitive Type: 0x" << std::hex << primType << std::dec << " - Cannot process detail attributes" << std::endl;
            return owned.release();
        }
        read<BIGEND>(*input, size);
        if(!*input || size < 0 || size > nPoints){
            bgeoFail(errorStream, "invalid or truncated BGEO particle primitive");
            return 0;
        }
        const std::uint64_t vertexBytes = std::uint64_t(size) * (nPoints >= (1 << 16) ? sizeof(int) : sizeof(unsigned short));
        if(!bgeoSkip(*input, vertexBytes + std::uint64_t(primSize) * sizeof(int))){
            bgeoFail(errorStream, "truncated BGEO particle primitive");
            return 0;
        }
    }

    int detailSize = 0;
    vector<int> fixedAttrOffsets;
    vector<FixedAttribute> fixedAttrHandles;
    if(!bgeoAttributes(*input, nAttrib, BgeoScope::Detail, simple, headersOnly, detailSize,
                       unusedPoints, fixedAttrHandles, fixedAttrOffsets, errorStream))
        return 0;
    vector<int> fixedBuffer(detailSize);
    const streamsize detailBytes = static_cast<streamsize>(fixedBuffer.size() * sizeof(int));
    if(detailBytes) input->read(reinterpret_cast<char*>(fixedBuffer.data()), detailBytes);
    if(!*input || (detailBytes && input->gcount() != detailBytes)){
        bgeoFail(errorStream, "truncated BGEO detail data");
        return 0;
    }
    if(!headersOnly)
        for(size_t attrIndex = 0; attrIndex < fixedAttrHandles.size(); attrIndex++)
            bgeoCopy(fixedBuffer, fixedAttrOffsets, int(attrIndex), fixedAttrHandles[attrIndex].count,
                     simple->fixedDataWrite<int>(fixedAttrHandles[attrIndex]));

    // return the populated simpleParticle
    return owned.release();
}
]=])
    file(WRITE "${output}" "${_bgeo_source}")
endfunction()
