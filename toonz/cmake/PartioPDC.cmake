# SPDX-License-Identifier: BSD-3-Clause
# Strict, bounded replacement for the pinned Partio Maya PDC reader.
# The generated source retains the upstream copyright, license and writer.
# The replaced reader span is identified by its SHA-256, so the patch fails
# when the reviewed upstream implementation changes.
function(ot_patch_partio_pdc source output)
    file(READ "${source}" _pdc_source)
    function(_pdc_replace old new)
        string(FIND "${_pdc_source}" "${old}" _pdc_match)
        if(_pdc_match LESS 0)
            message(FATAL_ERROR "Pinned Partio PDC source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _pdc_source "${_pdc_source}")
        set(_pdc_source "${_pdc_source}" PARENT_SCOPE)
    endfunction()
    # Replace the text from `first` through the end of `last` after verifying it.
    function(_pdc_rewrite first last digest new)
        string(FIND "${_pdc_source}" "${first}" _pdc_begin)
        string(FIND "${_pdc_source}" "${last}" _pdc_end)
        if(_pdc_begin LESS 0 OR _pdc_end LESS _pdc_begin)
            message(FATAL_ERROR "Pinned Partio PDC source no longer matches its reviewed patch")
        endif()
        string(LENGTH "${last}" _pdc_last)
        math(EXPR _pdc_length "${_pdc_end} + ${_pdc_last} - ${_pdc_begin}")
        string(SUBSTRING "${_pdc_source}" ${_pdc_begin} ${_pdc_length} _pdc_span)
        string(SHA256 _pdc_digest "${_pdc_span}")
        if(NOT _pdc_digest STREQUAL digest)
            message(FATAL_ERROR "Pinned Partio PDC reader no longer matches its reviewed patch")
        endif()
        string(SUBSTRING "${_pdc_source}" 0 ${_pdc_begin} _pdc_head)
        math(EXPR _pdc_tail_start "${_pdc_begin} + ${_pdc_length}")
        string(SUBSTRING "${_pdc_source}" ${_pdc_tail_start} -1 _pdc_tail)
        set(_pdc_source "${_pdc_head}${new}${_pdc_tail}" PARENT_SCOPE)
    endfunction()
    _pdc_replace([=[#include "io.h"]=] [=[#include "io.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>]=])
    # Upstream looped over an unchecked attribute count after the stream had
    # failed, allocated unchecked name lengths, left unknown attribute types
    # with no size and ignored truncated particle data.
    _pdc_rewrite([=[namespace {
std::string readName(std::istream& input){]=] [=[    return simple;
}
]=] 56b622b6916e9dc8541ad0477debccd4e0408a44f3e21f9e9d5bb6379cdf88ea [=[namespace Partio{

using namespace std;

static const int PDC_MAGIC = (((((' '<<8)|'C')<<8)|'D')<<8)|'P'; // " CDP"

typedef struct{
    int magic;
    int version;
    int bitorder;
    int tmp1;
    int tmp2;
    int numParticles;
    int numAttrs;
} PDC_HEADER;

namespace {
const int pdcMaxParticles = 10000000;
const int pdcMaxAttributes = 256;
const std::uint64_t pdcMaxBytes = 1073741824u;

bool pdcFail(std::ostream* errorStream, const char* message)
{
    if(errorStream) *errorStream << "Partio: " << message << endl;
    return false;
}

bool pdcSkip(istream& input, std::uint64_t bytes)
{
    while(bytes){
        const std::uint64_t chunk = (std::min)(bytes, std::uint64_t(1) << 20);
        input.ignore(static_cast<streamsize>(chunk));
        if(!input || static_cast<std::uint64_t>(input.gcount()) != chunk) return false;
        bytes -= chunk;
    }
    return true;
}

bool pdcName(istream& input, string& name)
{
    int nameLength = -1;
    read<BIGEND>(input, nameLength);
    if(!input || nameLength < 1 || nameLength > 65536) return false;
    name.assign(std::size_t(nameLength), '\0');
    input.read(&name[0], nameLength);
    return input && input.gcount() == nameLength && name.find('\0') == string::npos;
}
}  // namespace

// OT: all-or-nothing PDC reader. Per-object values (types 0, 2 and 4) and
// integer arrays (type 1) are skipped; double and vector arrays are loaded.
ParticlesDataMutable* readPDC(const char* filename, const bool headersOnly,std::ostream* errorStream){

    unique_ptr<istream> input(io::unzip(filename));
    if(!input || !*input){
        if(errorStream) *errorStream  << "Partio: Unable to open file " << filename << std::endl;
        return 0;
    }

    PDC_HEADER header{};
    input->read((char*)&header, sizeof(header));
    if(!*input || input->gcount() != std::streamsize(sizeof(header))){
        pdcFail(errorStream, "truncated PDC header");
        return 0;
    }
    if(PDC_MAGIC != header.magic){
        if(errorStream) *errorStream << "Partio: Magic number '" << header.magic << "' of '" << filename << "' doesn't match pdc magic '" << PDC_MAGIC << "'" << std::endl;
        return 0;
    }

    BIGEND::swap(header.numParticles);
    BIGEND::swap(header.numAttrs);
    if(header.numParticles < 0 || header.numParticles > pdcMaxParticles ||
       header.numAttrs < 0 || header.numAttrs > pdcMaxAttributes){
        pdcFail(errorStream, "invalid PDC particle or attribute count");
        return 0;
    }

    ParticlesDataMutable* simple = headersOnly ? new ParticleHeaders: create();
    unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    simple->addParticles(header.numParticles);

    const std::uint64_t count = std::uint64_t(header.numParticles);
    std::uint64_t components = 0;
    std::vector<char> buffer;
    for(int attrIndex = 0; attrIndex < header.numAttrs; attrIndex++){
        string attrName;
        int type = -1;
        if(!pdcName(*input, attrName)){
            pdcFail(errorStream, "invalid or truncated PDC attribute name");
            return 0;
        }
        read<BIGEND>(*input, type);
        if(!*input){
            pdcFail(errorStream, "truncated PDC attribute type");
            return 0;
        }
        // Element bytes of the stored value(s), and whether it is per particle.
        std::uint64_t bytes = 0;
        int dimensions = 0;
        switch(type){
            case 0: bytes = 4; break;
            case 1: bytes = 4 * count; break;
            case 2: bytes = 8; break;
            case 3: bytes = 8 * count; dimensions = 1; break;
            case 4: bytes = 24; break;
            case 5: bytes = 24 * count; dimensions = 3; break;
            default:
                pdcFail(errorStream, "unknown PDC attribute type");
                return 0;
        }
        if(!dimensions){
            if(!pdcSkip(*input, bytes)){
                pdcFail(errorStream, "truncated PDC attribute data");
                return 0;
            }
            if(errorStream) *errorStream << "Partio: skipped PDC attribute '" << attrName << "' of type " << type << std::endl;
            continue;
        }
        ParticleAttribute existing;
        if(simple->attributeInfo(attrName.c_str(), existing)){
            pdcFail(errorStream, "duplicate PDC attribute name");
            return 0;
        }
        components += std::uint64_t(dimensions);
        if(components * count * sizeof(float) > pdcMaxBytes){
            pdcFail(errorStream, "PDC particle data exceeds the memory limit");
            return 0;
        }
        const ParticleAttribute attr = simple->addAttribute(attrName.c_str(), dimensions == 3 ? VECTOR : FLOAT, dimensions);
        if(headersOnly){
            if(!pdcSkip(*input, bytes)){
                pdcFail(errorStream, "truncated PDC attribute data");
                return 0;
            }
            continue;
        }
        buffer.resize(std::size_t(bytes));
        if(bytes) input->read(buffer.data(), streamsize(bytes));
        if(!*input || (bytes && input->gcount() != streamsize(bytes))){
            pdcFail(errorStream, "truncated PDC attribute data");
            return 0;
        }
        for(int partIndex = 0; partIndex < header.numParticles; partIndex++){
            float* data = simple->dataWrite<float>(attr, partIndex);
            for(int dim = 0; dim < dimensions; dim++){
                double value;
                std::memcpy(&value, &buffer[(std::size_t(partIndex) * dimensions + dim) * sizeof(double)], sizeof(double));
                BIGEND::swap(value);
                if(!(std::fabs(value) <= double(FLT_MAX))){
                    pdcFail(errorStream, "PDC value is outside the float range");
                    return 0;
                }
                data[dim] = float(value);
            }
        }
    }

    return owned.release();
}
]=])
    file(WRITE "${output}" "${_pdc_source}")
endfunction()
