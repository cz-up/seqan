// ==========================================================================
//                         Mason - A Read Simulator
// ==========================================================================
// Copyright (c) 2006-2012, Knut Reinert, FU Berlin
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Knut Reinert or the FU Berlin nor the names of
//       its contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL KNUT REINERT OR THE FU BERLIN BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.
//
// ==========================================================================
// Author: Manuel Holtgrewe <manuel.holtgrewe@fu-berlin.de>
// ==========================================================================
// Simulate sequencing process from a genome.
// ==========================================================================

// TODO(holtgrew): Because of const holder issues, there are problems with passing strings as const.
// TODO(holtgrew): We should use bulk-reading calls to avoid indirect/virtual function calls.

#include <vector>
#include <utility>

#include "fragment_generation.h"
#include "sequencing.h"
#include "mason_options.h"
#include "mason_types.h"
#include "vcf_materialization.h"
#include "external_split_merge.h"

// ==========================================================================
// Forwards
// ==========================================================================

template <typename TMDString, typename TGaps1, typename TGaps2>
inline void getMDString2(TMDString &md, TGaps1 &gaps1, TGaps2 &gaps2);

// ==========================================================================
// Classes
// ==========================================================================

// --------------------------------------------------------------------------
// Class SingleEndRecordBuilder
// --------------------------------------------------------------------------

// Build the single-end records.
//
// Put into its own class to facilitate splitting into smaller functions.

class SingleEndRecordBuilder
{
public:
    // The sequencing simulation information to use.
    SequencingSimulationInfo & info;
    // The read sequence; state, restored after call
    seqan::Dna5String & seq;
    // State for integer to string conversion and such.
    std::stringstream & ss;
    seqan::CharString & buffer;
    // Quality string of our class.
    seqan::CharString const & qual;
    // Position map to use.
    PositionMap const & posMap;
    // Reference name and sequence.
    seqan::CharString const & refName;
    seqan::Dna5String /*const*/ & refSeq;
    // ID of reference, haplotype, and fragment.
    int rID, hID, fID;

    SingleEndRecordBuilder(SequencingSimulationInfo & info,
                           seqan::Dna5String & seq,
                           std::stringstream & ss,
                           seqan::CharString & buffer,
                           seqan::CharString const & qual,
                           PositionMap const & posMap,
                           seqan::CharString const & refName,
                           seqan::Dna5String /*const*/ & refSeq,
                           int rID, int hID, int fID) :
            info(info), seq(seq), ss(ss), buffer(buffer), qual(qual), posMap(posMap), refName(refName), refSeq(refSeq),
            rID(rID), hID(hID), fID(fID)
    {}

    // Fills all members of record except for qName which uses shared logic in ReadSimulatorThread.
    void build(seqan::BamAlignmentRecord & record)
    {
        _initialize(record);

        // Get length of alignment in reference.
        int len = 0;
        _getLengthInRef(info.cigar, len);

        // Compute whether the alignment overlaps with a breakpoint.
        bool overlapsWithBreakpoint = posMap.overlapsWithBreakpoint(info.beginPos, info.beginPos + len);

        // Get genomic interval that the mapping is on.
        GenomicInterval gi;
        if (!overlapsWithBreakpoint)
            gi = posMap.getGenomicInterval(info.beginPos);

        // Fill fields depending on being aligned/unaligned record.
        if (overlapsWithBreakpoint || gi.kind == GenomicInterval::INSERTED)
            _fillUnaligned(record, overlapsWithBreakpoint);
        else
            _fillAligned(record, len);
    }

    // Reset the record to be empty and reset records used for paired-end info.
    void _initialize(seqan::BamAlignmentRecord & record)
    {
        // Reset record.
        clear(record);

        // Mark clear single-end fields.
        record.flag = 0;
        record.rNextId = seqan::BamAlignmentRecord::INVALID_REFID;
        record.pNext = seqan::BamAlignmentRecord::INVALID_POS;
        record.tLen = seqan::BamAlignmentRecord::INVALID_LEN;

        // Update info and set query name.
        info.rID = rID;
        info.hID = hID;
    }

    // Fill the record's members for an unaligned record.
    void _fillUnaligned(seqan::BamAlignmentRecord & record, bool overlapsWithBreakpoint)
    {
        // Record for unaligned single-end read.
        record.flag = seqan::BAM_FLAG_UNMAPPED;
        record.rID = seqan::BamAlignmentRecord::INVALID_REFID;
        record.beginPos = seqan::BamAlignmentRecord::INVALID_POS;
        record.seq = seq;
        record.qual = qual;

        // Write out some tags with the information.
        seqan::BamTagsDict tagsDict(record.tags);
        // Set tag with the eason for begin unmapped: Inserted or over breakpoint.  We only reach here if the alignment
        // does not overlap with a breakpoint in the case that the alignment is in an inserted region.
        setTagValue(tagsDict, "uR", overlapsWithBreakpoint ? 'B' : 'I');
        // Set position on original haplotype.
        setTagValue(tagsDict, "oR", toCString(refName));  // original reference name
        setTagValue(tagsDict, "oP", info.beginPos);       // original position
        setTagValue(tagsDict, "oH", hID + 1);             // original haplotype
        setTagValue(tagsDict, "oS", info.isForward ? 'F' : 'R');  // original strand
    }

    // Fill the record's members for an aligned record.
    void _fillAligned(seqan::BamAlignmentRecord & record,
                      int len = 0)
    {
        // Convert from coordinate system with SVs to coordinate system with small variants.
        std::pair<int, int> intSmallVar = posMap.toSmallVarInterval(info.beginPos, info.beginPos + len);
        bool isRC = intSmallVar.first > intSmallVar.second;
        if (isRC)
            std::swap(intSmallVar.first, intSmallVar.second);
        // Convert from small variant coordinate system to original interval.
        std::pair<int, int> intOriginal = posMap.toOriginalInterval(intSmallVar.first, intSmallVar.second);

        _flipState(info.isForward == isRC);  // possibly flip state

        // Set the RC flag in the record.
        if (info.isForward == isRC)
            record.flag |= seqan::BAM_FLAG_RC;

        // Perform the alignment to compute the edit distance and the CIGAR string.
        int editDistance = 0;
        _alignAndSetCigar(record, editDistance, buffer, seq, intOriginal.first, intOriginal.second);

        // Set the remaining flags.
        record.rID = rID;
        record.beginPos = intOriginal.first;
        record.seq = seq;
        record.qual = qual;

        _flipState(info.isForward == isRC);  // restore state if previously flipped

        // Fill BAM tags.
        _fillTags(record, editDistance, buffer);
    }

    // Flip the sequence and quality in case that the record is reverse complemented.
    void _flipState(bool doFlip)
    {
        if (doFlip)
        {
            reverseComplement(seq);
            reverse(qual);
            reverse(info.cigar);
        }
    }

    // Perform the realignment and set cigar string.
    void _alignAndSetCigar(seqan::BamAlignmentRecord & record,
                           int & editDistance,
                           seqan::CharString & mdString,
                           seqan::Dna5String & seq,
                           int beginPos,
                           int endPos)
    {
        // Realign the read sequence against the original interval.
        typedef seqan::Infix<seqan::Dna5String>::Type TContigInfix;
        TContigInfix contigInfix(refSeq, beginPos, endPos);
        seqan::Gaps<TContigInfix> gapsContig(contigInfix);
        seqan::Gaps<seqan::Dna5String> gapsRead(seq);
        seqan::Score<int, seqan::Simple> sScheme(0, -1000, -1001, -1002);

        int buffer = 3;  // should be unnecessary
        int uDiag = std::max((int)(length(contigInfix) - length(seq)), 0) + buffer;
        int lDiag = -std::max((int)(length(seq) - length(contigInfix)), 0) - buffer;

        editDistance = globalAlignment(gapsContig, gapsRead, sScheme, lDiag, uDiag);
        editDistance /= -1000;  // score to edit distance

        getCigarString(record.cigar, gapsContig, gapsRead, seqan::maxValue<int>());
        getMDString2(mdString, gapsContig, gapsRead);
    }

    // Fill the tags dict.
    void _fillTags(seqan::BamAlignmentRecord & record,
                   int editDistance,
                   seqan::CharString const & mdString)
    {
        seqan::BamTagsDict tagsDict(record.tags);
        setTagValue(tagsDict, "NM", editDistance);        // edit distance to reference
        setTagValue(tagsDict, "MD", toCString(mdString));
        setTagValue(tagsDict, "oR", toCString(refName));  // original reference name
        setTagValue(tagsDict, "oH", hID + 1);             // original haplotype
        setTagValue(tagsDict, "oP", info.beginPos);       // original position
        setTagValue(tagsDict, "oS", info.isForward ? 'F' : 'R');  // original strand
    }
};

// --------------------------------------------------------------------------
// Class PairedEndRecordBuilder
// --------------------------------------------------------------------------

// Build the single-end records.
//
// Put into its own class to facilitate splitting into smaller functions.

class PairedEndRecordBuilder
{
public:
    // The sequencing simulation information to use.
    SequencingSimulationInfo & infoL;
    SequencingSimulationInfo & infoR;
    // The read sequences; state, restored after call.
    seqan::Dna5String & seqL;
    seqan::Dna5String & seqR;
    // State for integer to string conversion and such.
    std::stringstream & ss;
    seqan::CharString & buffer;
    // Quality strings.
    seqan::CharString & qualL;
    seqan::CharString & qualR;
    // Position map to use for coordinate conversion.
    PositionMap const & posMap;
    // Reference name and sequence.
    seqan::CharString const & refName;
    seqan::Dna5String /*const*/ & refSeq;
    // ID of teh reference, haplotype, and fragment.
    int rID, hID, fID;

    PairedEndRecordBuilder(SequencingSimulationInfo & infoL,
                           SequencingSimulationInfo & infoR,
                           seqan::Dna5String & seqL,
                           seqan::Dna5String & seqR,
                           std::stringstream & ss,
                           seqan::CharString & buffer,
                           seqan::CharString & qualL,
                           seqan::CharString & qualR,
                           PositionMap const & posMap,
                           seqan::CharString const & refName,
                           seqan::Dna5String /*const*/ & refSeq,
                           int rID, int hID, int fID) :
            infoL(infoL), infoR(infoR), seqL(seqL), seqR(seqR), ss(ss), buffer(buffer), qualL(qualL), qualR(qualR),
            posMap(posMap), refName(refName), refSeq(refSeq), rID(rID), hID(hID), fID(fID)
    {}

    // Fills all record members, excdept for qName which uses shared logic in ReadSimulatorThread.
    void build(seqan::BamAlignmentRecord & recordL,
               seqan::BamAlignmentRecord & recordR)
    {
        _initialize(recordL, recordR);

        // Get length of alignments in reference.
        int lenL = 0, lenR = 0;
        _getLengthInRef(infoL.cigar, lenL);
        _getLengthInRef(infoR.cigar, lenR);

        // Compute whether the left/right alignmetn overlaps with a breakpoint.
        bool overlapsWithBreakpointL = posMap.overlapsWithBreakpoint(infoL.beginPos, infoL.beginPos + lenL);
        bool overlapsWithBreakpointR = posMap.overlapsWithBreakpoint(infoR.beginPos, infoR.beginPos + lenR);

        // Get genomic intervals that the mappings are on.
        GenomicInterval giL, giR;
        if (!overlapsWithBreakpointL)
            giL = posMap.getGenomicInterval(infoL.beginPos);
        if (!overlapsWithBreakpointR)
            giR = posMap.getGenomicInterval(infoR.beginPos);

        // Shortcuts.
        bool unmappedL = (overlapsWithBreakpointL || giL.kind == GenomicInterval::INSERTED);
        bool unmappedR = (overlapsWithBreakpointR || giR.kind == GenomicInterval::INSERTED);

        // Fill single fields depending on being aligned/unaligned record.
        if (unmappedL)
            _fillUnaligned(recordL, infoL, seqL, qualL, overlapsWithBreakpointL);
        else
            _fillAligned(recordL, infoL, seqL, qualL, lenL);
        if (unmappedR)
            _fillUnaligned(recordR, infoR, seqR, qualR, overlapsWithBreakpointR);
        else
            _fillAligned(recordR, infoR, seqR, qualR, lenR);

        // -------------------------------------------------------------------
        // Complete flags and tLen.
        // -------------------------------------------------------------------
        //
        // This is surprising complex.
        recordL.flag |= seqan::BAM_FLAG_FIRST | seqan::BAM_FLAG_MULTIPLE;
        recordR.flag |= seqan::BAM_FLAG_LAST  | seqan::BAM_FLAG_MULTIPLE;

        if (!unmappedL && !unmappedR)
        {
            recordL.flag |= seqan::BAM_FLAG_ALL_PROPER;
            recordR.flag |= seqan::BAM_FLAG_ALL_PROPER;
            if (recordL.rID == recordR.rID)
            {
                if (recordL.beginPos < recordR.beginPos)
                    recordL.tLen = recordR.beginPos + lenR - recordL.beginPos;
                else
                    recordL.tLen = recordL.beginPos + lenL - recordR.beginPos;
                recordR.tLen = -recordL.tLen;
            }
            else
            {
                recordL.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
                recordR.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
            }

            recordL.rNextId = recordR.rID;
            recordL.pNext = recordR.beginPos;
            recordR.rNextId = recordL.rID;
            recordR.pNext = recordL.beginPos;

            if (hasFlagRC(recordL))
                recordR.flag |= seqan::BAM_FLAG_NEXT_RC;
            if (hasFlagRC(recordR))
                recordL.flag |= seqan::BAM_FLAG_NEXT_RC;
        }
        else if (!unmappedL && unmappedR)
        {
            recordR.rID = recordL.rID;
            recordR.beginPos = recordL.beginPos;
            recordR.flag |= seqan::BAM_FLAG_UNMAPPED;
            recordL.flag |= seqan::BAM_FLAG_NEXT_UNMAPPED;

            recordL.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
            recordR.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
        }
        else if (unmappedL && !unmappedR)
        {
            recordL.rID = recordR.rID;
            recordL.beginPos = recordR.beginPos;
            recordL.flag |= seqan::BAM_FLAG_UNMAPPED;
            recordR.flag |= seqan::BAM_FLAG_NEXT_UNMAPPED;

            recordL.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
            recordR.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
        }
        else if (unmappedL && unmappedR)
        {
            recordL.flag |= seqan::BAM_FLAG_UNMAPPED;
            recordR.flag |= seqan::BAM_FLAG_NEXT_UNMAPPED;
            recordL.flag |= seqan::BAM_FLAG_UNMAPPED;
            recordR.flag |= seqan::BAM_FLAG_NEXT_UNMAPPED;
        }
    }

    // Reset the record to be empty and reset records used for paired-end info.
    void _initialize(seqan::BamAlignmentRecord & recordL, seqan::BamAlignmentRecord & recordR)
    {
        // Reset record.
        clear(recordL);
        clear(recordR);

        // Mark clear single-end fields.
        recordL.flag = 0;
        recordL.rNextId = seqan::BamAlignmentRecord::INVALID_REFID;
        recordL.pNext = seqan::BamAlignmentRecord::INVALID_POS;
        recordL.tLen = seqan::BamAlignmentRecord::INVALID_LEN;
        recordR.flag = 0;
        recordR.rNextId = seqan::BamAlignmentRecord::INVALID_REFID;
        recordR.pNext = seqan::BamAlignmentRecord::INVALID_POS;
        recordR.tLen = seqan::BamAlignmentRecord::INVALID_LEN;

        // Update info and set query name.
        infoL.rID = rID;
        infoL.hID = hID;
        infoR.rID = rID;
        infoR.hID = hID;
    }

    // Fill the record's members for an unaligned record.
    void _fillUnaligned(seqan::BamAlignmentRecord & record,
                        SequencingSimulationInfo & infoRecord,
                        seqan::Dna5String const & seq,
                        seqan::CharString const & qual,
                        bool overlapsWithBreakpoint)
    {
        // Record for unaligned single-end read.
        record.flag = seqan::BAM_FLAG_UNMAPPED;
        record.rID = seqan::BamAlignmentRecord::INVALID_REFID;
        record.beginPos = seqan::BamAlignmentRecord::INVALID_POS;
        record.seq = seq;
        record.qual = qual;

        // Write out some tags with the information.
        seqan::BamTagsDict tagsDict(record.tags);
        // Set tag with the eason for begin unmapped: Inserted or over breakpoint.  We only reach here if the alignment
        // does not overlap with a breakpoint in the case that the alignment is in an inserted region.
        setTagValue(tagsDict, "uR", overlapsWithBreakpoint ? 'B' : 'I');
        // Set position on original haplotype.
        setTagValue(tagsDict, "oR", toCString(refName));  // original reference name
        setTagValue(tagsDict, "oP", infoRecord.beginPos);       // original position
        setTagValue(tagsDict, "oH", hID + 1);             // original haplotype
        setTagValue(tagsDict, "oS", infoRecord.isForward ? 'F' : 'R');  // original strand
    }

    // Flip the sequence and quality in case that the record is reverse complemented.
    void _flipState(SequencingSimulationInfo & infoRecord,
                    seqan::Dna5String & seq,
                    seqan::CharString & qual,
                    bool doFlip)
    {
        if (doFlip)
        {
            reverseComplement(seq);
            reverse(qual);
            reverse(infoRecord.cigar);
        }
    }

    // Fill the record's members for an aligned record.
    void _fillAligned(seqan::BamAlignmentRecord & record,
                      SequencingSimulationInfo & infoRecord,
                      seqan::Dna5String & seq,  // state, restored
                      seqan::CharString & qual, // state, restored
                      int len = 0)
    {
        // Convert from coordinate system with SVs to coordinate system with small variants.
        std::pair<int, int> intSmallVar = posMap.toSmallVarInterval(infoRecord.beginPos,
                                                                    infoRecord.beginPos + len);
        bool isRC = intSmallVar.first > intSmallVar.second;
        if (isRC)
            std::swap(intSmallVar.first, intSmallVar.second);
        // Convert from small variant coordinate system to original interval.
        std::pair<int, int> intOriginal = posMap.toOriginalInterval(intSmallVar.first, intSmallVar.second);

        _flipState(infoRecord, seq, qual, infoRecord.isForward == isRC);  // possibly flip state

        // Set the RC flag in the record.
        if (infoRecord.isForward == isRC)
            record.flag |= seqan::BAM_FLAG_RC;

        // Perform the alignment to compute the edit distance and the CIGAR string.
        int editDistance = 0;
        _alignAndSetCigar(record, editDistance, buffer, seq, intOriginal.first, intOriginal.second);

        // Set the remaining flags.
        record.rID = rID;
        record.beginPos = intOriginal.first;
        record.seq = seq;
        record.qual = qual;

        _flipState(infoRecord, seq, qual, infoRecord.isForward == isRC);  // restore state if previously flipped

        // Fill BAM tags.
        _fillTags(record, infoRecord, editDistance, buffer);
    }

    // Perform the realignment and set cigar string.
    void _alignAndSetCigar(seqan::BamAlignmentRecord & record,
                           int & editDistance,
                           seqan::CharString & mdString,
                           seqan::Dna5String & seq,
                           int beginPos,
                           int endPos)
    {
        // Realign the read sequence against the original interval.
        typedef seqan::Infix<seqan::Dna5String>::Type TContigInfix;
        TContigInfix contigInfix(refSeq, beginPos, endPos);
        seqan::Gaps<TContigInfix> gapsContig(contigInfix);
        seqan::Gaps<seqan::Dna5String> gapsRead(seq);
        seqan::Score<int, seqan::Simple> sScheme(0, -1000, -1001, -1002);

        int buffer = 3;  // should be unnecessary
        int uDiag = std::max((int)(length(contigInfix) - length(seq)), 0) + buffer;
        int lDiag = -std::max((int)(length(seq) - length(contigInfix)), 0) - buffer;

        editDistance = globalAlignment(gapsContig, gapsRead, sScheme, lDiag, uDiag);
        editDistance /= -1000;  // score to edit distance

        getCigarString(record.cigar, gapsContig, gapsRead, seqan::maxValue<int>());
        getMDString2(mdString, gapsContig, gapsRead);
    }

    // Fill the tags dict.
    void _fillTags(seqan::BamAlignmentRecord & record,
                   SequencingSimulationInfo & infoRecord,
                   int editDistance,
                   seqan::CharString const & mdString)
    {
        seqan::BamTagsDict tagsDict(record.tags);
        setTagValue(tagsDict, "NM", editDistance);        // edit distance to reference
        setTagValue(tagsDict, "MD", toCString(mdString));
        setTagValue(tagsDict, "oR", toCString(refName));  // original reference name
        setTagValue(tagsDict, "oH", hID + 1);             // original haplotype
        setTagValue(tagsDict, "oP", infoRecord.beginPos);       // original position
        setTagValue(tagsDict, "oS", infoRecord.isForward ? 'F' : 'R');  // original strand
    }
};

// --------------------------------------------------------------------------
// Class ReadSimulatorThread
// --------------------------------------------------------------------------

// State for one thread for simulation of reads.

class ReadSimulatorThread
{
public:
    // Options for the read simulation.
    MasonSimulatorOptions const * options;

    // The random number generator to use for this thread.
    TRng rng;

    // The ids of the fragments.
    std::vector<int> fragmentIds;

    // The fragment generator and fragment buffer.
    std::vector<Fragment> fragments;
    FragmentSampler * fragSampler;

    // Methylation levels to use, points to empty levels if methylation is disabled.
    MethylationLevels const * methLevels;

    // The sequencing simulator to use.
    SequencingSimulator * seqSimulator;

    // Buffer with ids and sequence of reads simulated in this thread.
    seqan::StringSet<seqan::CharString> ids;
    seqan::StringSet<seqan::Dna5String> seqs;
    seqan::StringSet<seqan::CharString> quals;
    std::vector<SequencingSimulationInfo> infos;
    // Buffer for the BAM alignment records.
    bool buildAlignments;  // Whether or not compute the BAM alignment records.
    std::vector<seqan::BamAlignmentRecord> alignmentRecords;

    ReadSimulatorThread() : options(), fragSampler(), methLevels(), seqSimulator(), buildAlignments(false)
    {}

    ~ReadSimulatorThread()
    {
        delete fragSampler;
        delete seqSimulator;
    }

    void init(int seed, MasonSimulatorOptions const & newOptions)
    {
        reSeed(rng, seed);
        options = &newOptions;
        buildAlignments = !empty(options->outFileNameSam);

        // Initialize fragment generator here with reference to RNG and options.
        fragSampler = new FragmentSampler(rng, options->fragSamplerOptions);

        // Create sequencing simulator.
        SequencingSimulatorFactory simFactory(rng, options->seqOptions, options->illuminaOptions,
                                              options->rocheOptions, options->sangerOptions);
        std::auto_ptr<SequencingSimulator> ptr = simFactory.make();
        seqSimulator = ptr.release();
    }

    void _setId(seqan::CharString & str, std::stringstream & ss, int fragId, int num,
                SequencingSimulationInfo const & info, bool forceNoEmbed = false)
    {
        ss.clear();
        ss.str("");
        ss << options->seqOptions.readNamePrefix;
        if (num == 0 || forceNoEmbed)
            ss << (fragId + 1);
        else if (num == 1)
            ss << (fragId + 1) << "/1";
        else  // num == 2
            ss << (fragId + 1) << "/2";
        if (options->seqOptions.embedReadInfo && !forceNoEmbed)
        {
            ss << ' ';
            info.serialize(ss);
        }
        str = ss.str();
    }

    void _simulatePairedEnd(seqan::Dna5String const & seq, PositionMap const & posMap,
                            seqan::CharString const & refName,
                            seqan::Dna5String /*const*/ & refSeq,
                            int rID, int hID)
    {
        std::stringstream ss;
        seqan::CharString buffer;

        for (unsigned i = 0; i < 2 * fragmentIds.size(); i += 2)
        {
            TFragment frag(seq, fragments[i / 2].beginPos, fragments[i / 2].endPos);
            seqSimulator->simulatePairedEnd(seqs[i], quals[i], infos[i],
                                            seqs[i + 1], quals[i + 1], infos[i + 1],
                                            frag, methLevels);
            infos[i].rID = infos[i + 1].rID = rID;
            infos[i].hID = infos[i + 1].hID = hID;
            // Set the sequence ids.
            _setId(ids[i], ss, fragmentIds[i / 2], 1, infos[i]);
            _setId(ids[i + 1], ss, fragmentIds[i / 2], 2, infos[i + 1]);

            if (buildAlignments)
            {
                // Build the alignment records themselves.
                PairedEndRecordBuilder builder(infos[i], infos[i + 1], seqs[i], seqs[i + 1], ss, buffer,
                                               quals[i], quals[i + 1], posMap, refName, refSeq,
                                               rID, hID, fragmentIds[i / 2]);
                builder.build(alignmentRecords[i], alignmentRecords[i + 1]);
                // Set qName members of alignment records.
                _setId(alignmentRecords[i].qName, ss, fragmentIds[i / 2], 1, infos[i], true);
                _setId(alignmentRecords[i + 1].qName, ss, fragmentIds[i / 2], 2, infos[i + 1], true);
            }
        }
    }

    void _simulateSingleEnd(seqan::Dna5String /*const*/ & seq,
                            PositionMap const & posMap,
                            seqan::CharString const & refName,
                            seqan::Dna5String /*const*/ & refSeq,
                            int rID, int hID)
    {
        std::stringstream ss;
        seqan::CharString buffer;

        for (unsigned i = 0; i < fragmentIds.size(); ++i)
        {
            TFragment frag(seq, fragments[i].beginPos, fragments[i].endPos);
            seqSimulator->simulateSingleEnd(seqs[i], quals[i], infos[i], frag, methLevels);
            _setId(ids[i], ss, fragmentIds[i], 0, infos[i]);
            if (buildAlignments)
            {
                // Build the alignment record itself.
                SingleEndRecordBuilder builder(infos[i], seqs[i], ss, buffer, quals[i],
                                               posMap, refName, refSeq, rID, hID, fragmentIds[i]);
                builder.build(alignmentRecords[i]);
                // Set query name.
                _setId(alignmentRecords[i].qName, ss, fragmentIds[i], 1, infos[i], true);
            }
        }
    }

    // Simulate next chunk.
    void run(seqan::Dna5String /*const*/ & seq,
             PositionMap const & posMap,
             seqan::CharString const & refName,
             seqan::Dna5String /*const*/ & refSeq,
             int rID, int hID)
    {
        // Sample fragments.
        fragSampler->generateMany(fragments, rID, length(seq), fragmentIds.size());

        // Simulate reads.
        int seqCount = (options->seqOptions.simulateMatePairs ? 2 : 1) * fragmentIds.size();
        resize(ids, seqCount);
        resize(seqs, seqCount);
        resize(quals, seqCount);
        infos.resize(seqCount);
        if (buildAlignments)
        {
            alignmentRecords.clear();
            alignmentRecords.resize(seqCount);
        }
        if (options->seqOptions.simulateMatePairs)
            _simulatePairedEnd(seq, posMap, refName, refSeq, rID, hID);
        else
            _simulateSingleEnd(seq, posMap, refName, refSeq, rID, hID);
    }
};

// --------------------------------------------------------------------------
// Class MasonSimulatorApp
// --------------------------------------------------------------------------

class MasonSimulatorApp
{
public:
    // The configuration to use for the simulation.
    MasonSimulatorOptions options;

    // The random number generator to use for the simulation and a separate one for the methylation levels when
    // materalizing the contig.
    TRng rng, methRng;

    // Threads used for simulation.
    std::vector<ReadSimulatorThread> threads;

    // ----------------------------------------------------------------------
    // VCF Materialization
    // ----------------------------------------------------------------------

    // Materialization of the contigs from a VCF file.
    VcfMaterializer vcfMat;
    // FAI Index for loading methylation levels.
    seqan::FaiIndex methFaiIndex;

    // ----------------------------------------------------------------------
    // Sample Source Distribution
    // ----------------------------------------------------------------------

    // Helper for distributing reads/pairs to contigs/haplotypes.
    ContigPicker contigPicker;
    // Helper for storing the read ids for each contig/haplotype pair.
    IdSplitter fragmentIdSplitter;
    // Helper for storing the simulated reads for each contig/haplotype pair.  We will write out SAM files with the
    // alignment information relative to the materialized sequence.
    IdSplitter fragmentSplitter;
    // Helper for joining the FASTQ files.
    std::auto_ptr<FastxJoiner<seqan::Fastq> > fastxJoiner;
    // Helper for storing SAM records for each contig/haplotype pair.  In the end, we will join this again.
    IdSplitter alignmentSplitter;
    // Helper for joining the SAM files.
    std::auto_ptr<SamJoiner> alignmentJoiner;

    // ----------------------------------------------------------------------
    // Header used for writing temporary SAM.
    // ----------------------------------------------------------------------

    typedef seqan::StringSet<seqan::CharString> TNameStore;
    typedef seqan::NameStoreCache<TNameStore>   TNameStoreCache;
    typedef seqan::BamIOContext<TNameStore>     TBamIOContext;
    TNameStore nameStore;
    seqan::BamHeader header;
    TNameStoreCache nameStoreCache;
    TBamIOContext   bamIOContext;

    // ----------------------------------------------------------------------
    // File Output
    // ----------------------------------------------------------------------

    // For writing left/right reads.
    seqan::SequenceStream outSeqsLeft, outSeqsRight;
    // For writing the final SAM/BAM file.
    seqan::BamStream outBamStream;

    MasonSimulatorApp(MasonSimulatorOptions const & options) :
            options(options), rng(options.seed), methRng(options.seed),
            vcfMat(methRng,
                   toCString(options.matOptions.fastaFileName),
                   toCString(options.matOptions.vcfFileName),
                   toCString(options.methFastaInFile),
                   &options.methOptions),
            contigPicker(rng), nameStoreCache(nameStore), bamIOContext(nameStore, nameStoreCache)
    {}

    int run()
    {
        // Print the header and the options.
        _printHeader();
        // Initialize.
        _init();
        // Simulate reads.
        _simulateReads();

        return 0;
    }

    void _simulateReadsDoSimulation()
    {
        std::cerr << "\nSimulating Reads:\n";
        int haplotypeCount = vcfMat.numHaplotypes;
        seqan::Dna5String contigSeq;  // materialized contig
        int rID = 0;  // current reference id
        int hID = 0;  // current haplotype id
        int contigFragmentCount = 0;  // number of reads on the contig
        // Note that all shared variables are correctly synchronized by implicit flushes at the critical sections below.
        MethylationLevels levels;
        seqan::Dna5String refSeq;  // reference sequence
        while ((options.seqOptions.bsSeqOptions.bsSimEnabled && vcfMat.materializeNext(contigSeq, levels, rID, hID)) ||
               (!options.seqOptions.bsSeqOptions.bsSimEnabled && vcfMat.materializeNext(contigSeq, rID, hID)))
        {
            std::cerr << "  " << sequenceName(vcfMat.faiIndex, rID) << " (allele " << (hID + 1) << ") ";
            contigFragmentCount = 0;
            if (readSequence(refSeq, vcfMat.faiIndex, rID) != 0)
                throw MasonIOException("Could not load reference sequence.");

            while (true)  // Execute as long as there are fragments left.
            {
                bool doBreak = false;
                for (int tID = 0; tID < options.numThreads; ++tID)
                {
                    // Read in the ids of the fragments to simulate.
                    threads[tID].fragmentIds.resize(options.chunkSize);  // make space
                    threads[tID].methLevels = &levels;

                    // Load the fragment ids to simulate for.
                    int numRead = fread(&threads[tID].fragmentIds[0], sizeof(int), options.chunkSize,
                                        fragmentIdSplitter.files[rID * haplotypeCount + hID]);
                    contigFragmentCount += numRead;
                    if (numRead == 0)
                        doBreak = true;
                    threads[tID].fragmentIds.resize(numRead);
                }

                // Perform the simulation.
                SEQAN_OMP_PRAGMA(parallel num_threads(options.numThreads))
                {
                    threads[omp_get_thread_num()].run(contigSeq, vcfMat.posMap,
                                                      sequenceName(vcfMat.faiIndex, rID),
                                                      refSeq, rID, hID);
                }

                // Write out the temporary sequence.
                for (int tID = 0; tID < options.numThreads; ++tID)
                {
                    if (write2(fragmentSplitter.files[rID * haplotypeCount + hID],
                               threads[tID].ids, threads[tID].seqs, threads[tID].quals, seqan::Fastq()))
                        throw MasonIOException("Could not write out temporary sequence.");
                    if (!empty(options.outFileNameSam))
                        for (unsigned i = 0; i < length(threads[tID].alignmentRecords); ++i)
                        {
                            if (write2(alignmentSplitter.files[rID * haplotypeCount + hID],
                                       threads[tID].alignmentRecords[i], bamIOContext, seqan::Sam()) != 0)
                                throw MasonIOException("Could not write out temporary alignment record.");
                        }
                    std::cerr << '.' << std::flush;
                }

                if (doBreak)
                    break;  // No more work left.
            }

            std::cerr << " (" << contigFragmentCount << " fragments) OK\n";
        }
        std::cerr << "  Done simulating reads.\n";
    }

    void _simulateReadsJoin()
    {
        std::cerr << "\nJoining temporary files ...";
        fragmentSplitter.reset();
        fastxJoiner.reset(new FastxJoiner<seqan::Fastq>(fragmentSplitter));
        FastxJoiner<seqan::Fastq> & joiner = *fastxJoiner.get();  // Shortcut
        seqan::CharString id, seq, qual;
        if (options.seqOptions.simulateMatePairs)
            while (!joiner.atEnd())
            {
                joiner.get(id, seq, qual);
                if (writeRecord(outSeqsLeft, id, seq, qual) != 0)
                    throw MasonIOException("Problem joining sequences.");
                joiner.get(id, seq, qual);
                if (writeRecord(outSeqsRight, id, seq, qual) != 0)
                    throw MasonIOException("Problem joining sequences.");
            }
        else
            while (!joiner.atEnd())
            {
                joiner.get(id, seq, qual);
                if (writeRecord(outSeqsLeft, id, seq, qual) != 0)
                    throw MasonIOException("Problem joining sequences.");
            }
        if (!empty(options.outFileNameSam))
        {
            alignmentSplitter.reset();
            alignmentJoiner.reset(new SamJoiner(alignmentSplitter));

            outBamStream.header = alignmentJoiner->header;

            SamJoiner & joiner = *alignmentJoiner.get();  // Shortcut
            seqan::BamAlignmentRecord record;
            while (!joiner.atEnd())
            {
                joiner.get(record);
                if (writeRecord(outBamStream, record) != 0)
                    throw MasonIOException("Problem writing to alignment out file.");
            }
        }
        std::cerr << " OK\n";
    }

    void _simulateReads()
    {
        std::cerr << "\n____READ SIMULATION___________________________________________________________\n"
                  << "\n";

        // (1) Distribute read ids to the contigs/haplotypes.
        //
        // We will simulate the reads in the order of contigs/haplotypes and in a final join script generate output file
        // that are sorted by read id.
        int seqCount = numSeqs(vcfMat.faiIndex);
        int haplotypeCount = vcfMat.numHaplotypes;
        std::cerr << "Distributing fragments to " << seqCount << " contigs (" << haplotypeCount
                  << " haplotypes each) ...";
        for (int i = 0; i < options.numFragments; ++i)
            fwrite(&i, sizeof(int), 1, fragmentIdSplitter.files[contigPicker.toId(contigPicker.pick())]);
        fragmentIdSplitter.reset();
        std::cerr << " OK\n";

        // (2) Simulate the reads in the order of contigs/haplotypes.
        _simulateReadsDoSimulation();

        // (3) Merge the sequences from external files into the output stream.
        _simulateReadsJoin();
    }

    // Initialize the alignment splitter data structure.
    void _initAlignmentSplitter()
    {
        // Open alignment splitters.
        alignmentSplitter.numContigs = fragmentIdSplitter.numContigs;
        alignmentSplitter.open();
        // Build and write out header, fill ref name store.
        seqan::BamHeaderRecord vnHeaderRecord;
        vnHeaderRecord.type = seqan::BAM_HEADER_FIRST;
        appendValue(vnHeaderRecord.tags, seqan::Pair<seqan::CharString>("VN", "1.4"));
        appendValue(header.records, vnHeaderRecord);
        resize(header.sequenceInfos, numSeqs(vcfMat.faiIndex));
        for (unsigned i = 0; i < numSeqs(vcfMat.faiIndex); ++i)
        {
            if (!empty(options.matOptions.vcfFileName))
                header.sequenceInfos[i].i1 = vcfMat.vcfStream.header.sequenceNames[i];
            else
                header.sequenceInfos[i].i1 = sequenceName(vcfMat.faiIndex, i);
            unsigned idx = 0;
            if (!getIdByName(vcfMat.faiIndex, header.sequenceInfos[i].i1, idx))
            {
                std::stringstream ss;
                ss << "Could not find " << header.sequenceInfos[i].i1 << " from VCF file in FAI index.";
                throw MasonIOException(ss.str());
            }
            header.sequenceInfos[i].i2 = sequenceLength(vcfMat.faiIndex, idx);
            appendValue(nameStore, sequenceName(vcfMat.faiIndex, idx));
            seqan::BamHeaderRecord seqHeaderRecord;
            seqHeaderRecord.type = seqan::BAM_HEADER_REFERENCE;
            appendValue(seqHeaderRecord.tags, seqan::Pair<seqan::CharString>("SN", header.sequenceInfos[i].i1));
            std::stringstream ss;
            ss << header.sequenceInfos[i].i2;
            appendValue(seqHeaderRecord.tags, seqan::Pair<seqan::CharString>("LN", ss.str().c_str()));
            appendValue(header.records, seqHeaderRecord);
        }
        refresh(nameStoreCache);
        for (unsigned i = 0; i < alignmentSplitter.files.size(); ++i)
            if (write2(alignmentSplitter.files[i], header, bamIOContext, seqan::Sam()) != 0)
                throw MasonIOException("Could not write out SAM header to temporary file.");
    }

    // Configure contigPicker.
    void _initContigPicker()
    {
        std::cerr << "Initializing fragment-to-contig distribution ...";
        // Contig picker.
        contigPicker.numHaplotypes = vcfMat.numHaplotypes;
        contigPicker.lengthSums.clear();
        for (unsigned i = 0; i < numSeqs(vcfMat.faiIndex); ++i)
        {
            contigPicker.lengthSums.push_back(sequenceLength(vcfMat.faiIndex, i));
            if (i > 0u)
                contigPicker.lengthSums[i] += contigPicker.lengthSums[i - 1];
        }
        // Fragment id splitter.
        fragmentIdSplitter.numContigs = numSeqs(vcfMat.faiIndex) * vcfMat.numHaplotypes;
        fragmentIdSplitter.open();
        // Splitter for sequence.
        fragmentSplitter.numContigs = fragmentIdSplitter.numContigs;
        fragmentSplitter.open();
        // Splitter for alignments, only required when writing out SAM/BAM.
        if (!empty(options.outFileNameSam))
            _initAlignmentSplitter();
        std::cerr << " OK\n";
    }

    // Open the output files.
    void _initOpenOutputFiles()
    {
        std::cerr << "Opening output file " << options.outFileNameLeft << " ...";
        open(outSeqsLeft, toCString(options.outFileNameLeft), seqan::SequenceStream::WRITE);
        outSeqsLeft.outputOptions = seqan::SequenceOutputOptions(0);  // also FASTA in one line
        if (!isGood(outSeqsLeft))
            throw MasonIOException("Could not open left/single-end output file.");
        std::cerr << " OK\n";

        if (!options.forceSingleEnd && !empty(options.outFileNameRight))
        {
            std::cerr << "Opening output file " << options.outFileNameRight << " ...";
            open(outSeqsRight, toCString(options.outFileNameRight), seqan::SequenceStream::WRITE);
            outSeqsRight.outputOptions = seqan::SequenceOutputOptions(0);  // also FASTA in one line
            if (!isGood(outSeqsRight))
                throw MasonIOException("Could not open right/single-end output file.");
            std::cerr << " OK\n";
        }

        if (!empty(options.outFileNameSam))
        {
            std::cerr << "Opening output file " << options.outFileNameSam << "...";
            open(outBamStream, toCString(options.outFileNameSam), seqan::BamStream::WRITE);
            if (!isGood(outBamStream))
                throw MasonIOException("Could not open SAM/BAM output file.");
            std::cerr << " OK\n";
        }
    }

    void _init()
    {
        std::cerr << "\n____INITIALIZING______________________________________________________________\n"
                  << "\n";

        // Initialize VCF materialization (reference FASTA and input VCF).
        std::cerr << "Opening reference and variants file ...";
        vcfMat.init();
        std::cerr << " OK\n";

        // Configure contigPicker and fragment id splitter.
        _initContigPicker();

        // Initialize simulation threads.
        std::cerr << "Initializing simulation threads ...";
        threads.resize(options.numThreads);
        for (int i = 0; i < options.numThreads; ++i)
            threads[i].init(options.seed + i * options.seedSpacing, options);
        std::cerr << " OK\n";

        // Open output files.
        _initOpenOutputFiles();
    }

    void _printHeader()
    {
        std::cerr << "MASON SIMULATOR\n"
                  << "===============\n";
        if (options.verbosity >= 2)
        {
            std::cerr << "\n";
            options.print(std::cerr);
        }
    }
};

// ==========================================================================
// Functions
// ==========================================================================

// --------------------------------------------------------------------------
// Function parseCommandLine()
// --------------------------------------------------------------------------

seqan::ArgumentParser::ParseResult
parseCommandLine(MasonSimulatorOptions & options, int argc, char const ** argv)
{
    // Setup ArgumentParser.
    seqan::ArgumentParser parser("mason_simulator");
    // Set short description, version, and date.
    setShortDescription(parser, "Read Simulation");
    setVersion(parser, "2.0");
    setDate(parser, "July 2012");
    setCategory(parser, "Simulators");

    // Define usage line and long description.
    addUsageLine(parser,
                 "[OPTIONS] \\fB-ir\\fP \\fIIN.fa\\fP \\fB-n\\fP \\fINUM\\fP [\\fB-iv\\fP \\fIIN.vcf\\fP] \\fB-o\\fP \\fILEFT.fq\\fP "
                 "[\\fB-or\\fP \\fIRIGHT.fq\\fP]");
    addDescription(parser,
                   "Simulate \\fINUM\\fP reads/pairs from the reference sequence \\fIIN.fa\\fP, potentially with "
                   "variants from \\fIIN.vcf\\fP.  In case that both \\fB-o\\fP and \\fB-or\\fP are given, write out "
                   "paired-end data, if only \\fB-io\\fP is given, only single-end reads are simulated.");

    // Add option and text sections.
    options.addOptions(parser);
    options.addTextSections(parser);

    // Parse command line.
    seqan::ArgumentParser::ParseResult res = seqan::parse(parser, argc, argv);

    // Only extract  options if the program will continue after parseCommandLine()
    if (res != seqan::ArgumentParser::PARSE_OK)
        return res;

    options.getOptionValues(parser);

    return seqan::ArgumentParser::PARSE_OK;
}

// ----------------------------------------------------------------------------
// getMDString2()
// ----------------------------------------------------------------------------

template <
    typename TMDString,
    typename TGaps1,
    typename TGaps2>
inline void
getMDString2(
    TMDString &md,
    TGaps1 &gaps1,
    TGaps2 &gaps2)
{
    typedef typename seqan::Value<TMDString>::Type TMDChar;
    typename seqan::Iterator<TGaps1>::Type it1 = begin(gaps1);
    typename seqan::Iterator<TGaps2>::Type it2 = begin(gaps2);
    typedef typename seqan::Value<typename seqan::Source<TGaps1>::Type>::Type TValue1;
    typedef typename seqan::Value<typename seqan::Source<TGaps2>::Type>::Type TValue2;
    char op, lastOp = ' ';
    unsigned numOps = 0;

    clear(md);
    for (; !atEnd(it1) && !atEnd(it2); goNext(it1), goNext(it2))
    {
        if (isGap(it1)) continue;
        if (isGap(it2))
        {
            op = 'D';
        }
        else
        {
            TValue1 x1 = *it1;
            TValue1 x2 = *it2;
            op = (x1 == x2)? 'M': 'R';
        }

        // append match run
        if (lastOp != op)
        {
            if (lastOp == 'M')
            {
                std::stringstream num;
                num << numOps;
                append(md, num.str());
            }
            numOps = 0;
            lastOp = op;
        }

        // append deleted/replaced reference character
        if (op != 'M')
        {
            // add ^ from non-deletion to deletion
            if (op == 'D' && lastOp != 'D')
                appendValue(md, '^');
            // add 0 from deletion to replacement
            if (op == 'R' && lastOp == 'D')
                appendValue(md, '0');
            appendValue(md, seqan::convert<TMDChar>(*it1));
        }

        ++numOps;
    }
    SEQAN_ASSERT_EQ(atEnd(it1), atEnd(it2));
    if (lastOp == 'M')
    {
        std::stringstream num;
        num << numOps;
        append(md, num.str());
    }
}

// --------------------------------------------------------------------------
// Function main()
// --------------------------------------------------------------------------

int main(int argc, char const ** argv)
{
    // Parse options.
    MasonSimulatorOptions options;
    seqan::ArgumentParser::ParseResult res = parseCommandLine(options, argc, argv);
    if (res != seqan::ArgumentParser::PARSE_OK)
        return res == seqan::ArgumentParser::PARSE_ERROR;

    // Initialize Global State
    //
    // Random number generator to use throughout mason.
    TRng rng(options.seed);

    // Run the application.
    MasonSimulatorApp app(options);
    return app.run();
}