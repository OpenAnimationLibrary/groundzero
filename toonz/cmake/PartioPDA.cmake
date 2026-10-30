# SPDX-License-Identifier: BSD-3-Clause
# Strict, bounded replacement for the pinned Partio ASCII PDA reader.
# The generated source retains the upstream copyright, license and writer.
# The replaced reader span is identified by its SHA-256, so the patch fails
# when the reviewed upstream implementation changes.
function(ot_patch_partio_pda source output)
    file(READ "${source}" _pda_source)
    function(_pda_replace old new)
        string(FIND "${_pda_source}" "${old}" _pda_match)
        if(_pda_match LESS 0)
            message(FATAL_ERROR "Pinned Partio PDA source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _pda_source "${_pda_source}")
        set(_pda_source "${_pda_source}" PARENT_SCOPE)
    endfunction()
    # Replace the text from `first` through the end of `last` after verifying it.
    function(_pda_rewrite first last digest new)
        string(FIND "${_pda_source}" "${first}" _pda_begin)
        string(FIND "${_pda_source}" "${last}" _pda_end)
        if(_pda_begin LESS 0 OR _pda_end LESS _pda_begin)
            message(FATAL_ERROR "Pinned Partio PDA source no longer matches its reviewed patch")
        endif()
        string(LENGTH "${last}" _pda_last)
        math(EXPR _pda_length "${_pda_end} + ${_pda_last} - ${_pda_begin}")
        string(SUBSTRING "${_pda_source}" ${_pda_begin} ${_pda_length} _pda_span)
        string(SHA256 _pda_digest "${_pda_span}")
        if(NOT _pda_digest STREQUAL digest)
            message(FATAL_ERROR "Pinned Partio PDA reader no longer matches its reviewed patch")
        endif()
        string(SUBSTRING "${_pda_source}" 0 ${_pda_begin} _pda_head)
        math(EXPR _pda_tail_start "${_pda_begin} + ${_pda_length}")
        string(SUBSTRING "${_pda_source}" ${_pda_tail_start} -1 _pda_tail)
        set(_pda_source "${_pda_head}${new}${_pda_tail}" PARENT_SCOPE)
    endfunction()
    _pda_replace([=[#include "io.h"]=] [=[#include "io.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>]=])
    # Upstream stopped reading particle values at the first stream error and
    # still returned the particles, so a truncated or malformed file loaded
    # uninitialized values; unknown types and unmatched columns were ignored.
    _pda_rewrite([=[ParticlesDataMutable* readPDA(]=] [=[    return simple;
}
]=] d6a37c4e2e4d69dab63a2e0170e4ad270283f535097c8de7070f5e89ebdc77bf [=[namespace {
const unsigned int pdaMaxParticles = 10000000;
const std::size_t pdaMaxAttributes = 256;
const std::uint64_t pdaMaxBytes = 1073741824u;

ParticlesDataMutable* pdaFail(std::ostream* errorStream, const char* message)
{
    if(errorStream) *errorStream << "Partio: " << message << endl;
    return 0;
}

// One whitespace-delimited token of at most 4096 bytes.
bool pdaToken(istream& input, string& token)
{
    token.clear();
    input >> ws;
    for(int c = input.peek(); c != char_traits<char>::eof() && !std::isspace(c); c = input.peek()){
        if(!c || token.size() >= 4096) return false;
        token += char(input.get());
    }
    return !input.bad() && !token.empty();
}

// A number must be a whole token: "1.5" in an integer column is invalid.
template<class T> bool pdaNumber(istream& input, T& value)
{
    if(!(input >> value)) return false;
    const int c = input.peek();
    return c == char_traits<char>::eof() || std::isspace(c);
}
}  // namespace

// OT: all-or-nothing PDA reader. Every declared particle value must parse.
ParticlesDataMutable* readPDA(const char* filename,const bool headersOnly,std::ostream* errorStream)
{
    unique_ptr<istream> input(io::unzip(filename));
    if(!input || !*input){
        if(errorStream) *errorStream <<"Partio: Can't open particle data file: "<<filename<<endl;
        return 0;
    }

    string word;
    if(!pdaToken(*input, word) || word != "ATTRIBUTES") return pdaFail(errorStream, "missing PDA ATTRIBUTES header");
    vector<string> attrNames;
    std::set<string> names;
    for(;;){
        if(!pdaToken(*input, word)) return pdaFail(errorStream, "truncated PDA attribute names");
        if(word == "TYPES") break;
        if(attrNames.size() >= pdaMaxAttributes || !names.insert(word).second)
            return pdaFail(errorStream, "too many or duplicate PDA attributes");
        attrNames.push_back(word);
    }
    vector<int> counts;
    vector<ParticleAttributeType> types;
    std::uint64_t components = 0;
    for(;;){
        if(!pdaToken(*input, word)) return pdaFail(errorStream, "truncated PDA attribute types");
        if(word == "NUMBER_OF_PARTICLES:") break;
        if(types.size() >= attrNames.size()) return pdaFail(errorStream, "PDA has more types than attributes");
        if(word == "V") { types.push_back(Partio::VECTOR); counts.push_back(3); }
        else if(word == "R") { types.push_back(Partio::FLOAT); counts.push_back(1); }
        else if(word == "I") { types.push_back(Partio::INT); counts.push_back(1); }
        else return pdaFail(errorStream, "unknown PDA attribute type");
        components += std::uint64_t(counts.back());
    }
    if(types.size() != attrNames.size()) return pdaFail(errorStream, "PDA has fewer types than attributes");

    unsigned int num = 0;
    if(!pdaToken(*input, word) || word.size() > 9 ||
       !std::all_of(word.begin(), word.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return pdaFail(errorStream, "invalid PDA particle count");
    for(char c : word) num = num * 10 + unsigned(c - '0');
    if(num > pdaMaxParticles || components * num * sizeof(float) > pdaMaxBytes)
        return pdaFail(errorStream, "PDA particle data exceeds the memory limit");
    if(!pdaToken(*input, word) || word != "BEGIN" || !pdaToken(*input, word) || word != "DATA")
        return pdaFail(errorStream, "missing PDA BEGIN DATA marker");

    ParticlesDataMutable* simple = headersOnly ? new ParticleHeaders : create();
    unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    vector<ParticleAttribute> attrs;
    for(size_t i = 0; i < attrNames.size(); i++)
        attrs.push_back(simple->addAttribute(attrNames[i].c_str(), types[i], counts[i]));
    if(headersOnly){
        simple->addParticles(int(num));
        return owned.release();
    }

    // Grow geometrically while parsing, so a short file cannot reserve the
    // full declared particle count before its data is known to exist.
    for(unsigned int particleIndex = 0; particleIndex < num; particleIndex++){
        if(particleIndex == unsigned(simple->numParticles()))
            simple->addParticles(int((std::min)(num - particleIndex, (std::max)(1024u, particleIndex))));
        for(size_t attrIndex = 0; attrIndex < attrs.size(); attrIndex++){
            if(attrs[attrIndex].type == Partio::INT){
                int* data = simple->dataWrite<int>(attrs[attrIndex], particleIndex);
                for(int count = 0; count < attrs[attrIndex].count; count++)
                    if(!pdaNumber(*input, data[count])) return pdaFail(errorStream, "invalid or truncated PDA particle data");
            }else{
                float* data = simple->dataWrite<float>(attrs[attrIndex], particleIndex);
                for(int count = 0; count < attrs[attrIndex].count; count++)
                    if(!pdaNumber(*input, data[count])) return pdaFail(errorStream, "invalid or truncated PDA particle data");
            }
        }
    }
    return owned.release();
}
]=])
    file(WRITE "${output}" "${_pda_source}")
endfunction()
