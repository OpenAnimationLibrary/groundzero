# SPDX-License-Identifier: BSD-3-Clause
# Bounds and completeness checks for the pinned Partio PTC reader.
# The generated source retains the upstream copyright and license notice.
# Exact replacements fail if the reviewed upstream implementation changes.
function(ot_patch_partio_ptc source output)
    file(READ "${source}" _ptc_source)
    macro(_ptc_replace old new)
        string(FIND "${_ptc_source}" "${old}" _ptc_match)
        if(_ptc_match LESS 0)
            message(FATAL_ERROR "Pinned Partio PTC source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _ptc_source "${_ptc_source}")
    endmacro()
    _ptc_replace([=[inline string GetString(istream& input,const char terminator='\0')
{
    // TODO: make this check for FEOF condition! and also more efficient
    char c=' ';
    string s;
    while (c!=terminator && input)
    {
        input.read(&c,sizeof(char));
	s += c;
    }
    return s;
}

bool ParseSpec(const string& spec,string& typeName,string& name)
{
    // TODO: make more robust
    const char* s=spec.c_str();
    typeName="";name="";
    while(*s!=' ') typeName+=*s++;
    while(*s==' ') s++;
    while(*s!='\n') name+=*s++;
    return true;
}

]=] [=[inline string GetString(istream& input,const char terminator='\0')
{
    string result;
    char c = 0;
    while (input.get(c)) {
        if (result.size() >= 1048576u) {
            input.setstate(ios::failbit);
            return string();
        }
        result += c;
        if (c == terminator) return result;
    }
    return string();
}

bool ParseSpec(const string& spec,string& typeName,string& name)
{
    const size_t separator = spec.find(' '), newline = spec.find('\n');
    if (separator == string::npos || separator == 0 || newline == string::npos ||
        newline != spec.size() - 1 || separator >= newline ||
        spec.find('\0') != string::npos) return false;
    const size_t start = spec.find_first_not_of(' ', separator);
    if (start == string::npos || start >= newline) return false;
    typeName = spec.substr(0, separator);
    name = spec.substr(start, newline - start);
    return true;
}

]=])
    _ptc_replace([=[    int magic;
    read<LITEND>(*input,magic);
    if(ptc_magic!=magic){]=] [=[    int magic = 0;
    read<LITEND>(*input,magic);
    if(!*input || ptc_magic!=magic){]=])
    _ptc_replace([=[    int version;
    read<LITEND>(*input,version);
    if(version>2){]=] [=[    int version = -1;
    read<LITEND>(*input,version);
    if(!*input || version < 0 || version>2){]=])
    _ptc_replace([=[    double nPoints;
    read<LITEND>(*input,nPoints);]=] [=[    double nPoints = 0;
    read<LITEND>(*input,nPoints);
    if (!*input || !std::isfinite(nPoints) || nPoints < 0 ||
        nPoints > 10000000 || std::floor(nPoints) != nPoints) {
        if(errorStream) *errorStream << "Invalid PTC particle count" << endl;
        return nullptr;
    }]=])
    _ptc_replace([=[    int nVars,dataSize;
    read<LITEND>(*input,nVars,dataSize);]=] [=[    int nVars = 0, dataSize = 0;
    read<LITEND>(*input,nVars,dataSize);
    if (!*input || nVars < 0 || nVars > 253 || dataSize < 0 ||
        dataSize > 253 * 16 ||
        (nPoints && nPoints > 1073741824.0 / sizeof(float) / (dataSize + 7))) {
        if(errorStream) *errorStream << "Invalid or truncated PTC channel table" << endl;
        return nullptr;
    }]=])
    _ptc_replace([=[    else simple=create();
    simple->addParticles]=] [=[    else simple=create();
    unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    simple->addParticles]=])
    _ptc_replace([=[        ParseSpec(GetString(*input,'\n'),typeName,name);]=] [=[        const string spec = GetString(*input, '\n');
        if (!*input || !ParseSpec(spec, typeName, name)) {
            if(errorStream) *errorStream << "Invalid or truncated PTC attribute specification" << endl;
            return nullptr;
        }]=])
    _ptc_replace([=[            simple->release();]=] [=[            // Particle storage is released by its owner.]=])
    _ptc_replace([=[        simple->release();]=] [=[        // Particle storage is released by its owner.]=])
    _ptc_replace([=[        return simple;]=] [=[        return owned.release();]=])
    _ptc_replace([=[    if(version>=1) for(int i=0;i<2;i++) read<LITEND>(*input,dummy);

    for(int pointIndex]=] [=[    if(version>=1) for(int i=0;i<2;i++) read<LITEND>(*input,dummy);
    if (!*input) {
        if(errorStream) *errorStream << "Truncated PTC particle data" << endl;
        return nullptr;
    }

    for(int pointIndex]=])
    _ptc_replace([=[        read<LITEND>(*input,phi,z);
        float* norm]=] [=[        read<LITEND>(*input,phi,z);
        if (!*input) {
            if(errorStream) *errorStream << "Truncated PTC particle data" << endl;
            return nullptr;
        }
        float* norm]=])
    _ptc_replace([=[                read<LITEND>(*input,data[j]);
        }

    }

    return simple;]=] [=[                read<LITEND>(*input,data[j]);
        }
        if (!*input) {
            if(errorStream) *errorStream << "Truncated PTC particle data" << endl;
            return nullptr;
        }
    }

    return owned.release();]=])
    file(WRITE "${output}" "${_ptc_source}")
endfunction()
