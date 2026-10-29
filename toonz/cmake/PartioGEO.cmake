# SPDX-License-Identifier: BSD-3-Clause
# Bounds and completeness checks for the pinned Partio GEO reader.
# The generated source retains the upstream copyright and license notice.
# Exact replacements fail if the reviewed upstream implementation changes.
function(ot_patch_partio_geo source output)
    file(READ "${source}" _geo_source)
    macro(_geo_replace old new)
        string(FIND "${_geo_source}" "${old}" _geo_match)
        if(_geo_match LESS 0)
            message(FATAL_ERROR "Pinned Partio GEO source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _geo_source "${_geo_source}")
    endmacro()
    _geo_replace([=[#include "io.h"]=] [=[#include "io.h"
#include <cctype>
#include <limits>]=])
    _geo_replace([=[string scanString(istream& input)
{
    // TODO: this code does not check for buf overrun
    // TODO: this code doesn't properly check for FEOF condition
    char buf[4096];
    char *ptr=buf;
    char c;
    while(input.good()){
        input.get(c);
        if(!isspace(c)) break;
    }
    if(!input.good()) return "";

    if(c!='"'){
        while(input.good()){
            *ptr++=c;
            input.get(c);
            if(isspace(c)) break;
        }
    }else{
        while(input.good()){
            input.get(c);
            if(c=='\\'){
                input.get(c);
                *ptr++=c;
            }else if(c=='"'){
                break;
            }else{
                *ptr++=c;
            }
        }
    }
    *ptr++=0;
    return string(buf);
}

]=] [=[string scanString(istream& input)
{
    // Grow a bounded string instead of writing past the old 4096-byte stack buffer.
    string token;
    char c = 0;
    input >> ws;
    if (!input.get(c)) return token;
    if (!c) { input.setstate(ios::failbit); return token; }
    const bool quoted = c == '"';
    if (!quoted) token += c;
    while (input.get(c)) {
        if (quoted && c == '"') return token;
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) return token;
        if (quoted && c == '\\' && !input.get(c)) return string();
        if (!c || token.size() >= 1073741824u) {
            input.setstate(ios::failbit);
            return string();
        }
        token += c;
    }
    // A quoted dictionary entry requires a closing quote, including after escapes.
    return string();
}

]=])
    _geo_replace([=[    else simple=create();

    // read NPoints and NPointAttrib]=] [=[    else simple=create();
    unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    auto invalid = [&]() -> ParticlesDataMutable* {
        if(errorStream) *errorStream << "Invalid or truncated GEO particle data" << endl;
        return nullptr;
    };

    // read NPoints and NPointAttrib]=])
    _geo_replace([=[    // read NPoints and NPointAttrib
    string word;
    while(input->good()){
        *input>>word;
        if(word=="NPoints") *input>>NPoints;
        else if(word=="NPointAttrib"){*input>>NPointAttrib;break;}
    }
    // skip until PointAttrib
    while(input->good()){
        *input>>word;
        if(word=="PointAttrib") break;
    }
]=] [=[    // Validate counts before creating attributes or allocating particle storage.
    string word;
    bool havePoints = false, haveAttributes = false;
    while (*input >> word) {
        if (word == "NPoints") { *input >> NPoints; havePoints = true; }
        else if (word == "NPointAttrib") {
            *input >> NPointAttrib; haveAttributes = true; break;
        }
    }
    if (!*input || !havePoints || !haveAttributes || NPoints < 0 ||
        NPoints > 10000000 || NPointAttrib < 0 || NPointAttrib > 255)
        return invalid();
    if (NPointAttrib) {
        while (*input >> word) if (word == "PointAttrib") break;
        if (!*input || word != "PointAttrib") return invalid();
    } else {
        // The writer omits PointAttrib when position is the only attribute.
        input->ignore((numeric_limits<streamsize>::max)(), '\n');
    }
]=])
    _geo_replace([=[    int attrInfoRead = 0;]=] [=[    int attrInfoRead = 0;
    size_t componentCount = 3, dictionaryBytes = 0;]=])
    _geo_replace([=[        *input >> attrName >> nvals >> attrType;
        if(attrType=="index"){]=] [=[        *input >> attrName >> nvals >> attrType;
        ParticleAttribute existing;
        if (!*input || attrName.empty() || attrName.find('\0') != string::npos ||
            simple->attributeInfo(attrName.c_str(), existing) || nvals < 1 ||
            nvals > 4096 || (attrType == "vector" && nvals != 3) ||
            (attrType == "index" && nvals != 1)) return invalid();
        componentCount += nvals;
        if (NPoints && componentCount > 1073741824u / sizeof(float) / NPoints)
            return invalid();
        if(attrType=="index"){]=])
    _geo_replace([=[            *input>>nIndices;
            ParticleAttribute attribute]=] [=[            *input>>nIndices;
            if (!*input || nIndices < 0 || nIndices > 10000000) return invalid();
            ParticleAttribute attribute]=])
    _geo_replace([=[                indexName=scanString(*input);
                if (!headersOnly) {]=] [=[                indexName=scanString(*input);
                if (!*input || indexName.size() > 1073741824u - dictionaryBytes)
                    return invalid();
                dictionaryBytes += indexName.size();
                if (!headersOnly) {]=])
    _geo_replace([=[                type=NONE;
            }
            attrs.push_back]=] [=[                return invalid();
            }
            if (!*input) return invalid();
            attrs.push_back]=])
    _geo_replace([=[    simple->addParticles(NPoints);]=] [=[    if (!*input || attrInfoRead != NPointAttrib) return invalid();
    simple->addParticles(NPoints);]=])
    _geo_replace([=[    if(headersOnly) return simple;]=] [=[    if(headersOnly) return owned.release();]=])
    _geo_replace([=[    for(ParticlesDataMutable::iterator end=simple->end();iterator!=end  && input->good();++iterator){]=] [=[    for(ParticlesDataMutable::iterator end=simple->end();iterator!=end;++iterator){]=])
    _geo_replace([=[        *input>> paren;
        if (paren != '(') break;]=] [=[        if (!*input) return invalid();
        if (!attrs.empty()) {
            *input >> paren;
            if (!*input || paren != '(') return invalid();
        }]=])
    _geo_replace([=[        *input >> paren;
        if (paren != ')') break;]=] [=[        if (!attrs.empty()) {
            *input >> paren;
            if (paren != ')') return invalid();
        }
        if (!*input) return invalid();]=])
    _geo_replace([=[    return simple;
}


template<class T>]=] [=[    return owned.release();
}


template<class T>]=])
    file(WRITE "${output}" "${_geo_source}")
endfunction()
