# SPDX-License-Identifier: BSD-3-Clause
# Strict, bounded replacement for the pinned Partio Maya cache (MC) reader.
# The generated source retains the upstream copyright and license notice.
# The replaced reader span is identified by its SHA-256, so the patch fails
# when the reviewed upstream implementation changes.
function(ot_patch_partio_mc source output)
    file(READ "${source}" _mc_source)
    function(_mc_replace old new)
        string(FIND "${_mc_source}" "${old}" _mc_match)
        if(_mc_match LESS 0)
            message(FATAL_ERROR "Pinned Partio MC source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _mc_source "${_mc_source}")
        set(_mc_source "${_mc_source}" PARENT_SCOPE)
    endfunction()
    # Replace the text from `first` through the end of `last` after verifying it.
    function(_mc_rewrite first last digest new)
        string(FIND "${_mc_source}" "${first}" _mc_begin)
        string(FIND "${_mc_source}" "${last}" _mc_end)
        if(_mc_begin LESS 0 OR _mc_end LESS _mc_begin)
            message(FATAL_ERROR "Pinned Partio MC source no longer matches its reviewed patch")
        endif()
        string(LENGTH "${last}" _mc_last)
        math(EXPR _mc_length "${_mc_end} + ${_mc_last} - ${_mc_begin}")
        string(SUBSTRING "${_mc_source}" ${_mc_begin} ${_mc_length} _mc_span)
        string(SHA256 _mc_digest "${_mc_span}")
        if(NOT _mc_digest STREQUAL digest)
            message(FATAL_ERROR "Pinned Partio MC reader no longer matches its reviewed patch")
        endif()
        string(SUBSTRING "${_mc_source}" 0 ${_mc_begin} _mc_head)
        math(EXPR _mc_tail_start "${_mc_begin} + ${_mc_length}")
        string(SUBSTRING "${_mc_source}" ${_mc_tail_start} -1 _mc_tail)
        set(_mc_source "${_mc_head}${new}${_mc_tail}" PARENT_SCOPE)
    endfunction()
    _mc_replace([=[#include "io.h"]=] [=[#include "io.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>]=])
    # Upstream looped while tellg() (-1 once the stream failed) was below the
    # block size, so a truncated file never terminated; GetString built a
    # string from an unterminated buffer; channel lengths were never checked.
    _mc_rewrite([=[std::string GetString(std::istream& input, unsigned int size){]=] [=[    }
    return simple;

}
]=] f537fe2247761f17df0814189a6257c2cf2162dfb84c0cc09f3bcfa86e1a3c65 [=[namespace {
const std::uint32_t mcMaxParticles = 10000000;
const int mcMaxChannels = 256;
const std::uint64_t mcMaxBytes = 1073741824u;

bool mcFail(std::ostream* errorStream, const char* message)
{
    if(errorStream) *errorStream << "Partio: " << message << std::endl;
    return false;
}

// Forward-only skip that requires every byte.
bool mcSkip(std::istream& input, std::uint64_t bytes)
{
    while(bytes){
        const std::uint64_t chunk = (std::min)(bytes, std::uint64_t(1) << 20);
        input.ignore(static_cast<std::streamsize>(chunk));
        if(!input || static_cast<std::uint64_t>(input.gcount()) != chunk) return false;
        bytes -= chunk;
    }
    return true;
}

bool mcTag(std::istream& input, char tag[4])
{
    input.read(tag, 4);
    return input && input.gcount() == 4;
}

struct McChannel {
    std::string name;
    char type[4];
    std::uint32_t count;
    std::uint32_t bytes;
    std::uint64_t dataOffset;  // From the start of the channel block contents.
};

// Positions the stream at the first channel of the MYCH block.
bool mcOpenBlock(std::istream& input, std::uint64_t& blockBytes, std::ostream* errorStream)
{
    char tag[4];
    std::int32_t headerSize = -1, blockSize = -1;
    if(!mcTag(input, tag) || std::memcmp(tag, "FOR4", 4))
        return mcFail(errorStream, "MC magic number doesn't match 'FOR4'");
    read<BIGEND>(input, headerSize);
    if(!input || headerSize < 4 || headerSize > (1 << 20) || headerSize % 4 || !mcSkip(input, std::uint64_t(headerSize)))
        return mcFail(errorStream, "invalid or truncated MC header");
    if(!mcTag(input, tag) || std::memcmp(tag, "FOR4", 4))
        return mcFail(errorStream, "missing MC data block");
    read<BIGEND>(input, blockSize);
    if(!input || blockSize < 4 || std::uint64_t(blockSize) > mcMaxBytes)
        return mcFail(errorStream, "invalid MC data block size");
    if(!mcTag(input, tag) || std::memcmp(tag, "MYCH", 4))
        return mcFail(errorStream, "missing MC channel block");
    blockBytes = std::uint64_t(blockSize) - 4;
    return true;
}

// Reads every channel header, skipping (and so verifying) the channel data.
bool mcChannels(std::istream& input, std::uint64_t blockBytes, std::vector<McChannel>& channels, std::ostream* errorStream)
{
    std::uint64_t used = 0;
    while(used < blockBytes){
        if(channels.size() >= std::size_t(mcMaxChannels))
            return mcFail(errorStream, "too many MC channels");
        McChannel channel;
        char tag[4];
        std::int32_t nameSize = -1, sizeBytes = -1;
        if(!mcTag(input, tag) || std::memcmp(tag, "CHNM", 4))
            return mcFail(errorStream, "invalid or truncated MC channel name tag");
        read<BIGEND>(input, nameSize);
        if(!input || nameSize < 1 || nameSize > 65536)
            return mcFail(errorStream, "invalid MC channel name size");
        const std::uint32_t padded = (std::uint32_t(nameSize) + 3u) & ~3u;
        std::vector<char> name(padded);
        input.read(name.data(), padded);
        if(!input || input.gcount() != std::streamsize(padded))
            return mcFail(errorStream, "truncated MC channel name");
        channel.name.assign(name.begin(), std::find(name.begin(), name.begin() + nameSize, '\0'));
        channel.name = channel.name.substr(channel.name.find_first_of("_") + 1);
        if(!mcTag(input, tag) || std::memcmp(tag, "SIZE", 4))
            return mcFail(errorStream, "invalid or truncated MC channel size tag");
        read<BIGEND>(input, sizeBytes);
        read<BIGEND>(input, channel.count);
        if(!input || sizeBytes != 4 || !mcTag(input, channel.type))
            return mcFail(errorStream, "invalid or truncated MC channel size");
        read<BIGEND>(input, channel.bytes);
        if(!input) return mcFail(errorStream, "truncated MC channel header");
        used += 28u + padded;
        channel.dataOffset = used;
        if(used > blockBytes || channel.bytes > blockBytes - used)
            return mcFail(errorStream, "MC channel exceeds its block");
        if(!mcSkip(input, channel.bytes))
            return mcFail(errorStream, "truncated MC channel data");
        used += channel.bytes;
        channels.push_back(channel);
    }
    return true;
}

bool mcFloat(double value, float& result)
{
    if(!(std::fabs(value) <= double(FLT_MAX))) return false;
    result = float(value);
    return true;
}
}  // namespace

// OT: forward-only, all-or-nothing Maya cache reader. The data pass reopens
// the file instead of seeking, so gzip input behaves like plain input.
ParticlesDataMutable* readMC(const char* filename, const bool headersOnly,std::ostream* errorStream){

    std::unique_ptr<std::istream> input(io::unzip(filename));
    if(!input || !*input){
        if(errorStream) *errorStream << "Partio: Unable to open file " << filename << std::endl;
        return 0;
    }
    std::uint64_t blockBytes = 0;
    std::vector<McChannel> channels;
    if(!mcOpenBlock(*input, blockBytes, errorStream) || !mcChannels(*input, blockBytes, channels, errorStream))
        return 0;

    // The particle count is the length of the id channel.
    bool haveId = false;
    std::uint32_t numParticles = 0;
    for(const McChannel& channel : channels){
        if(channel.name != "id") continue;
        if(haveId){
            mcFail(errorStream, "duplicate MC id channel");
            return 0;
        }
        haveId = true;
        numParticles = channel.count;
    }
    if(!haveId || numParticles > mcMaxParticles){
        mcFail(errorStream, "missing MC id channel or invalid particle count");
        return 0;
    }

    ParticlesDataMutable* simple = headersOnly ? static_cast<ParticlesDataMutable*>(new ParticleHeaders) : create();
    std::unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    std::vector<ParticleAttribute> attributes(channels.size());
    std::uint64_t components = 0;
    for(std::size_t i = 0; i < channels.size(); i++){
        const McChannel& channel = channels[i];
        const bool doubles = !std::memcmp(channel.type, "DBLA", 4);
        const bool vectors = !std::memcmp(channel.type, "FVCA", 4);
        if(!doubles && !vectors){
            if(errorStream) *errorStream << "Partio: Attribute '" << channel.name << " " << std::string(channel.type, 4) << "' cannot map type" << std::endl;
            continue;
        }
        if(std::uint64_t(channel.bytes) != std::uint64_t(channel.count) * (doubles ? 8u : 12u)){
            mcFail(errorStream, "MC channel size doesn't match its element count");
            return 0;
        }
        // Single values describe the object (for example its count), not particles.
        if(channel.count != numParticles){
            if(channel.count == 1) continue;
            mcFail(errorStream, "MC channel length doesn't match the particle count");
            return 0;
        }
        ParticleAttribute existing;
        if(channel.name.empty() || channel.name.find('\0') != std::string::npos ||
           simple->attributeInfo(channel.name.c_str(), existing)){
            mcFail(errorStream, "empty or duplicate MC channel name");
            return 0;
        }
        components += vectors ? 3 : 1;
        if(components * numParticles * sizeof(float) > mcMaxBytes){
            mcFail(errorStream, "MC particle data exceeds the memory limit");
            return 0;
        }
        attributes[i] = simple->addAttribute(channel.name.c_str(), vectors ? VECTOR : (channel.name == "id" ? INT : FLOAT), vectors ? 3 : 1);
    }
    simple->addParticles(int(numParticles));
    if(headersOnly) return owned.release();

    input = io::unzip(filename);
    if(!input || !*input || !mcOpenBlock(*input, blockBytes, errorStream)){
        mcFail(errorStream, "cannot reopen MC file");
        return 0;
    }
    std::uint64_t position = 0;
    std::vector<char> buffer;
    for(std::size_t i = 0; i < channels.size(); i++){
        const ParticleAttribute& attribute = attributes[i];
        if(attribute.attributeIndex < 0) continue;
        const McChannel& channel = channels[i];
        if(!mcSkip(*input, channel.dataOffset - position)){
            mcFail(errorStream, "truncated MC channel data");
            return 0;
        }
        buffer.resize(channel.bytes);
        if(channel.bytes) input->read(buffer.data(), channel.bytes);
        if(!*input || (channel.bytes && input->gcount() != std::streamsize(channel.bytes))){
            mcFail(errorStream, "truncated MC channel data");
            return 0;
        }
        position = channel.dataOffset + channel.bytes;
        for(std::uint32_t particle = 0; particle < numParticles; particle++){
            if(attribute.type == VECTOR){
                float* data = simple->dataWrite<float>(attribute, particle);
                for(int k = 0; k < 3; k++){
                    float value;
                    std::memcpy(&value, &buffer[(std::size_t(particle) * 3 + k) * sizeof(float)], sizeof(float));
                    BIGEND::swap(value);
                    data[k] = value;
                }
                continue;
            }
            double value;
            std::memcpy(&value, &buffer[std::size_t(particle) * sizeof(double)], sizeof(double));
            BIGEND::swap(value);
            if(attribute.type == INT){
                if(!(value >= double((std::numeric_limits<int>::min)()) &&
                     value <= double((std::numeric_limits<int>::max)())) || std::floor(value) != value){
                    mcFail(errorStream, "MC particle id is not a 32-bit integer");
                    return 0;
                }
                simple->dataWrite<int>(attribute, particle)[0] = int(value);
            }else if(!mcFloat(value, simple->dataWrite<float>(attribute, particle)[0])){
                mcFail(errorStream, "MC value is outside the float range");
                return 0;
            }
        }
    }
    return owned.release();
}
]=])
    file(WRITE "${output}" "${_mc_source}")
endfunction()
