# SPDX-License-Identifier: BSD-3-Clause
# Strict header, payload and integer checks for the pinned Partio BIN reader.
# The generated source retains the upstream copyright and license notice.
function(ot_patch_partio_bin source output)
    file(READ "${source}" _bin_source)
    function(_bin_replace old new)
        string(FIND "${_bin_source}" "${old}" _bin_match)
        if(_bin_match LESS 0)
            message(FATAL_ERROR "Pinned Partio BIN source no longer matches its reviewed patch")
        endif()
        string(REPLACE "${old}" "${new}" _bin_source "${_bin_source}")
        set(_bin_source "${_bin_source}" PARENT_SCOPE)
    endfunction()
    _bin_replace([=[#include "io.h"]=] [=[#include "io.h"
#include <cstdint>
#include <limits>]=])
    _bin_replace([=[    if(!*input){]=] [=[    if(!input || !*input){]=])
    _bin_replace([=[    BIN_HEADER header;
    input->read((char*)&header, sizeof(BIN_HEADERV6));]=] [=[    BIN_HEADER header{};
    input->read((char*)&header, sizeof(BIN_HEADERV6));
    if (!*input) {
        if(errorStream) *errorStream << "Truncated BIN header" << endl;
        return nullptr;
    }]=])
    _bin_replace([=[    // After version 13, we don't know what to do
    if(header.version > 13){
        cerr << "Partio: Unknown .bin version : " << header.version << endl;
        return 0;
    }]=] [=[    if (!*input || header.version < 1 || header.version > 13 ||
        header.numParticles < 0 || header.numParticles > 10000000) {
        if(errorStream) *errorStream << "Invalid or truncated BIN header, version or count" << endl;
        return nullptr;
    }]=])
    _bin_replace([=[    ParticlesDataMutable* simple = headersOnly ? new ParticleHeaders: create();
    simple->addParticles(header.numParticles);]=] [=[    // BIN v1-13 has fixed-width records, plus optional trailing RF data.
    // Validate minimum payload size with forward reads, including in header-only
    // mode. This does not require a seekable or uncompressed input stream.
    const std::uint64_t recordBytes = 68u +
        (header.version >= 3 ? 12u : 0u) +
        (header.version >= 4 ? 4u : 0u) +
        (header.version >= 5 ? 14u : 0u) +
        (header.version >= 9 ? 12u : 0u) +
        (header.version >= 12 ? 4u : 0u);
    const std::uint64_t payloadBytes = recordBytes *
        static_cast<std::uint64_t>(header.numParticles);
    if (payloadBytes > 1073741824ull) {
        if(errorStream) *errorStream << "BIN particle data exceeds the memory limit" << endl;
        return nullptr;
    }
    if (headersOnly) {
        input->ignore(static_cast<streamsize>(payloadBytes));
        if (!*input || static_cast<std::uint64_t>(input->gcount()) != payloadBytes) {
            if(errorStream) *errorStream << "Truncated BIN particle data" << endl;
            return nullptr;
        }
    }
    ParticlesDataMutable* simple = headersOnly ? new ParticleHeaders: create();
    unique_ptr<ParticlesDataMutable, void(*)(ParticlesDataMutable*)> owned(
        simple, [](ParticlesDataMutable* p) { if (p) p->release(); });
    simple->addParticles(header.numParticles);]=])
    _bin_replace([=[                // Warning, cast the id on 32 bits here
                uint64_t pid64;
                input->read ((char*) &pid64, sizeof(pid64));
                pid = (int)pid64;]=] [=[                std::uint64_t pid64 = 0;
                input->read ((char*) &pid64, sizeof(pid64));
                if (!*input) {
                    if(errorStream) *errorStream << "Truncated BIN particle ID" << endl;
                    return nullptr;
                }
                if (pid64 > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
                    if(errorStream) *errorStream << "BIN particle ID exceeds Partio's 32-bit integer representation" << endl;
                    return nullptr;
                }
                pid = static_cast<int>(pid64);]=])
    _bin_replace([=[            simple->dataWrite<int>(pidAttr, partIndex)[0] = (int)pid;]=] [=[            if (!*input) {
                if(errorStream) *errorStream << "Truncated BIN particle data" << endl;
                return nullptr;
            }
            simple->dataWrite<int>(pidAttr, partIndex)[0] = pid;]=])
    _bin_replace([=[    return simple;
}

bool writeBIN]=] [=[    return owned.release();
}

bool writeBIN]=])
    file(WRITE "${output}" "${_bin_source}")
endfunction()
