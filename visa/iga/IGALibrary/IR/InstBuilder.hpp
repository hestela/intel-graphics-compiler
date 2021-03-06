/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/
#ifndef _IGA_INST_BUILDER_HPP_
#define _IGA_INST_BUILDER_HPP_

#include "Kernel.hpp"
#include "../asserts.hpp"
#include "../ErrorHandler.hpp"
#include "../Frontend/IRToString.hpp"
#include "../Models/Models.hpp"

#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace iga
{
// The IR Builder is called by various instruction generators that build
// full instructions from partial state.  This allows us to separate IR
// construction from the actual syntax processing or decoding.
//
// This is needed since we have to save information about the operands
// during the parse.  We can't create operands as we see them since we don't
// have an existing Instruction * until InstEnd().   E.g. createSend
// requires us having seen exDesc and desc.  Since these follow the operands;
// hence, we must save operand info as state as we go.  This allows us to
// treat instructions as more immutable entities rather than complex state
// machines (via mutation) and allows us to be a little more sure that
// Instruction values are correct states.
//
// A better decision would be to make the IR types mutable by at least this
// class
struct OperandInfo
{
    Loc                      loc;
    Operand::Kind            kind = Operand::Kind::INVALID;

    union // optional modifier (e.g. -r12, ~r12, (abs) (sat))
    {
        SrcModifier  regOpSrcMod = SrcModifier::NONE;
        DstModifier  regOpDstMod;
    };
    RegName        regOpName = RegName::INVALID;    // e.g. r#, a#, null, ...
    Region         regOpRgn = Region::INVALID;     // e.g. <1>, <8;8,1>
    MathMacroExt   regOpMathMacroExtReg = MathMacroExt::INVALID; // e.g. math macro spc acc

    // direct/indirect register info
    RegRef   regOpReg; // direct operands

    // indirect register offset
    int16_t  regOpIndOff = 0; // e.g. "16" in "r[a0.4,16]"

    // imm field
    ImmVal   immValue;

    std::string              immLabel;
    Type                     type = Type::INVALID;

    OperandInfo()
    {
        kind = Operand::Kind::INVALID;
        regOpSrcMod = SrcModifier::NONE;
        regOpName = RegName::INVALID;
        regOpRgn = Region::INVALID;
        regOpMathMacroExtReg = MathMacroExt::INVALID;
        regOpReg = REGREF_ZERO_ZERO;
        regOpIndOff = 0;
        immValue.u64 = 0;
        immValue.kind = ImmVal::UNDEF;
        immLabel.clear();
        type = Type::INVALID;
    }

    void reset()
    {
        kind = Operand::Kind::INVALID;
        regOpSrcMod = SrcModifier::NONE;
        regOpName = RegName::INVALID;
        regOpRgn = Region::INVALID;
        regOpMathMacroExtReg = MathMacroExt::INVALID;
        regOpReg = REGREF_ZERO_ZERO;
        regOpIndOff = 0;
        immValue.u64 = 0;
        immValue.kind = ImmVal::UNDEF;
        immLabel.clear();
        type = Type::INVALID;
    }
};


// Larger constructs, such as blocks, instructions, and whatnot
// generally consist of a ***Start() and ***End() method to denote the
// start and end of each feature.  Upon parse or semantic error, the
// parser may terminate early, and corresponding ***End() functions may
// not be called in those cases.
//
// The methods are roughly called in syntax order, but this isn't a strict
// requirement and the handler should not make any assumptions about that.
class InstBuilder {
    const Model&                m_model;
    ErrorHandler&               m_errorHandler;
    Kernel                     *m_kernel;

    // per-instruction state
    Loc                         m_loc;
    Predication                 m_predication;
    const OpSpec               *m_opSpec = nullptr;
    BranchCntrl                 m_brnchCtrl = BranchCntrl::OFF;

    RegRef                      m_flagReg; // shared by predication / condition modifier

    ExecSize                    m_execSize = ExecSize::INVALID;
    ChannelOffset               m_chOff    = ChannelOffset::M0;
    MaskCtrl                    m_maskCtrl = MaskCtrl::NOMASK;

    FlagModifier                m_flagModifier = FlagModifier::NONE;
    DstModifier                 m_dstModifier  = DstModifier::NONE;

    OperandInfo                 m_dst;
    OperandInfo                 m_srcs[3];
    int                         m_nSrcs = 0;

    SendDescArg                 m_exDesc;
    SendDescArg                 m_desc;
    InstOptSet                  m_instOpts;

    std::string                 m_comment;

    ////////////////////////////////////////////////////////////////////
    // During parse, we add all instructions to one list, record label
    // locations and unresolved operands.  At the end of the parse,
    // we'll resolve everything to numeric form and then split things into
    // blocks at the end (if desired)
    //
    // one full linear list of instructions,
    InstList                    m_insts;
    //
    // labels defined (block starts)
    // (start-loc,start-pc,end-loc,end-pc
    using LabelInfo=std::tuple<Loc,uint32_t>;
    std::map<std::string,LabelInfo>  m_labelMap;
    LabelInfo                       *m_currBlock = nullptr;
    // unresolved operand labels
    struct UnresolvedLabel {
        Loc             loc;
        std::string     symbol;
        Operand        &operand;
        Instruction    &inst;
    };
    std::vector<UnresolvedLabel>     m_unresolvedLabels;

    uint32_t                    m_pc = 0; // current PC
    uint32_t                    m_nextId = 0; // next instruction id

    SWSB_ENCODE_MODE            m_swsbEncodeMode;

public:
    struct SWSBInfo {
        enum DIST_TYPE {
            NONE,
            REG_DIST,
        };
        DIST_TYPE distType = NONE;
        uint32_t regDist = 0;      // e.g. @3       0 = not set
        int32_t memSBidAlloc = -1; // e.g. $2      -1 = not set
        int32_t memSBidDst = -1;   // e.g. $2.dst  -1 = not set
        int32_t memSBidSrc = -1;   // e.g. $2.src  -1 = not set
        SWSBInfo() { } // sets default values as above
        bool anyBarrierSet() const {
            return sbidAllocSet() || sbidDstSet() || sbidSrcSet();
        }
        bool sbidAllocSet() const {return memSBidAlloc >= 0;}
        bool sbidDstSet() const {return memSBidDst >= 0;}
        bool sbidSrcSet() const {return memSBidSrc >= 0;}
    };

private:
    SWSBInfo                    m_depInfo;

    void clearInstState() {
        m_predication.function = PredCtrl::NONE;
        m_predication.inverse = false;

        m_flagReg = REGREF_ZERO_ZERO;

        m_opSpec = nullptr;

        m_execSize = ExecSize::SIMD1;
        m_chOff = ChannelOffset::M0;
        m_brnchCtrl = BranchCntrl::OFF;
        m_maskCtrl = MaskCtrl::NORMAL;

        m_flagModifier = FlagModifier::NONE;

        m_dstModifier = DstModifier::NONE;

        m_dst.reset();
        for (auto &m_src : m_srcs) {
            m_src.reset();
        }
        m_nSrcs = 0;

        m_exDesc.imm = 0;
        m_desc.imm = 0;

        m_instOpts.clear();
        m_depInfo = SWSBInfo();

        m_comment.clear();
    }

public:
    InstBuilder(Kernel *kernel, ErrorHandler &e)
        : m_model(kernel->getModel())
        , m_errorHandler(e)
        , m_kernel(kernel)
    {
        m_swsbEncodeMode = m_model.getSWSBEncodeMode();
    }

    InstList &getInsts() {return m_insts;}

    ErrorHandler &errorHandler() {return m_errorHandler;}

    void setSWSBEncodingMode(SWSB_ENCODE_MODE mode)
    {
        m_swsbEncodeMode = mode;
    }

    // Called at the beginning of the program
    void ProgramStart() {
        m_pc = 0;
        m_nextId = 1;
    }


    void ProgramEnd() {
        for (const UnresolvedLabel &u : m_unresolvedLabels) {
            auto itr = m_labelMap.find(u.symbol);
            if (itr == m_labelMap.end()) {
                m_errorHandler.reportError(u.loc, "undefined label");
            } else {
                const LabelInfo &li = itr->second;
                int32_t val = (int32_t)std::get<1>(li);
                if (!u.inst.getOpSpec().isJipAbsolute()) {
                    val -= u.inst.getPC();
                }
                u.operand.setLabelSource(val, u.operand.getType());
            }
        }
        // at this point m_instList has instructions with all labels in
        // numeric form
    }


    void BlockStart(const Loc &loc, const std::string &label) {
        auto itr = m_labelMap.find(label);
        if (itr != m_labelMap.end()) {
            std::stringstream err;
            err << "label redefinition " << label << " (defined "
                << "on line " << std::get<0>(itr->second).line << ")";
            m_errorHandler.reportError(loc, err.str());
        } else {
            m_labelMap[label] = LabelInfo(loc,m_pc);
            m_currBlock = &m_labelMap[label];
        }
    }


    void BlockEnd(uint32_t extent) {
        if (m_currBlock) {
            // could be null if error in BlockStart
            std::get<0>(*m_currBlock).extent = extent;
            m_currBlock = nullptr;
        }
    }


    void InstStart(const Loc &loc) {
        clearInstState();
        m_loc = loc;
    }


    void InstEnd(uint32_t extent) {
        // set the full instruction length in chars (for text)
        // or bytes (for decoding bits)
        m_loc.extent = extent;

        Instruction *inst = nullptr;
        if (m_opSpec->isMathSubFunc()) {
            inst =
                m_kernel->createBasicInstruction(
                    *m_opSpec,
                    m_predication,
                    m_flagReg,
                    m_execSize,
                    m_chOff,
                    m_maskCtrl,
                    m_flagModifier);
        } else if (m_opSpec->format == OpSpec::Format::SYNC_UNARY) {
            inst =
                m_kernel->createBasicInstruction(
                    *m_opSpec,
                    m_predication,
                    m_flagReg,
                    m_execSize,
                    m_chOff,
                    m_maskCtrl,
                    FlagModifier::NONE);
        } else if (m_opSpec->isBranching()) {
            inst =
                m_kernel->createBranchInstruction(
                    *m_opSpec,
                    m_predication,
                    m_flagReg,
                    m_execSize,
                    m_chOff,
                    m_maskCtrl,
                    m_brnchCtrl);
        } else if (m_opSpec->isSendOrSendsFamily()) {
            inst =
                m_kernel->createSendInstruction(
                    *m_opSpec,
                    m_predication,
                    m_flagReg,
                    m_execSize,
                    m_chOff,
                    m_maskCtrl,
                    m_exDesc,
                    m_desc);
        } else if (m_opSpec->op == Op::NOP) {
            inst = m_kernel->createNopInstruction();
        } else if (m_opSpec->op == Op::ILLEGAL) {
            inst = m_kernel->createIllegalInstruction();
        } else {
            inst =
                m_kernel->createBasicInstruction(
                    *m_opSpec,
                    m_predication,
                    m_flagReg,
                    m_execSize,
                    m_chOff,
                    m_maskCtrl,
                    m_flagModifier);
        }
        inst->setLoc(m_loc);
        m_insts.emplace_back(inst);

        if (m_opSpec->supportsDestination()) {
            if (m_dst.kind == Operand::Kind::DIRECT) {
                inst->setDirectDestination(
                    m_dstModifier,
                    m_dst.regOpName,
                    m_dst.regOpReg,
                    m_dst.regOpRgn.getHz(),
                    m_dst.type);
            } else if (m_dst.kind == Operand::Kind::MACRO) {
                inst->setMacroDestination(
                    m_dstModifier,
                    m_dst.regOpName,
                    m_dst.regOpReg,
                    m_dst.regOpMathMacroExtReg,
                    m_dst.regOpRgn.getHz(),
                    m_dst.type);
            } else { // Operand::Kind::INDIRECT
                inst->setInidirectDestination(
                    m_dstModifier,
                    m_dst.regOpReg,
                    m_dst.regOpIndOff,
                    m_dst.regOpRgn.getHz(),
                    m_dst.type);
            }
        } // end setting destinations

        // set source operands
        for (int i = 0; i < m_nSrcs; i++) {
            const OperandInfo &src = m_srcs[i];
            SourceIndex opIx = (SourceIndex)((int)SourceIndex::SRC0 + i);
            if (src.kind == Operand::Kind::DIRECT) {
                inst->setDirectSource(
                    opIx,
                    src.regOpSrcMod,
                    src.regOpName,
                    src.regOpReg,
                    src.regOpRgn,
                    src.type);
            } else if (src.kind == Operand::Kind::MACRO) {
                inst->setMacroSource(
                    opIx,
                    src.regOpSrcMod,
                    src.regOpName,
                    src.regOpReg,
                    src.regOpMathMacroExtReg,
                    src.regOpRgn,
                    src.type);
            } else if (src.kind == Operand::Kind::INDIRECT) {
                inst->setInidirectSource(
                    opIx,
                    src.regOpSrcMod,
                    src.regOpReg,
                    src.regOpIndOff,
                    src.regOpRgn,
                    src.type);
            } else if (src.kind == Operand::Kind::LABEL) {
                if (src.immLabel.empty()) {
                    // numeric label was used
                    inst->setLabelSource(opIx, src.immValue.s32, src.type);
                } else {
                    // label (unresolved)
                    //
                    // we'll backpatch later, but set it for the type
                    inst->setLabelSource(opIx, 0, src.type);
                    UnresolvedLabel u {
                        src.loc,
                        src.immLabel,
                        inst->getSource(opIx),
                        *inst};
                    m_unresolvedLabels.push_back(u);
                }
            } else if (src.kind == Operand::Kind::IMMEDIATE) {
                inst->setImmediateSource(opIx, src.immValue, src.type);
            } else {
                IGA_ASSERT_FALSE("unexpected src kind");
            }
        } // for
        inst->addInstOpts(m_instOpts);
        inst->setID(m_nextId++);
        inst->setPC(m_pc);
        if (!m_comment.empty()) {
            inst->setComment(m_comment);
        }


        SWSB swInfo;
        // this assumes checks for incompatible stuff are done during parsing
        if (m_depInfo.sbidSrcSet())
        {
            swInfo.sbid = m_depInfo.memSBidSrc;
            swInfo.tokenType = SWSB::TokenType::SRC;
        }
        if (m_depInfo.sbidDstSet())
        {
            swInfo.sbid = m_depInfo.memSBidDst;
            swInfo.tokenType = SWSB::TokenType::DST;
        }
        if (m_depInfo.sbidAllocSet())
        {
            swInfo.sbid = m_depInfo.memSBidAlloc;
            swInfo.tokenType = SWSB::TokenType::SET;
        }

        if (m_depInfo.regDist > 0)
        {
            SWSB::DistType swsb_type = SWSB::DistType::NO_DIST;
            switch(m_depInfo.distType) {
            case SWSBInfo::REG_DIST:
                swsb_type = SWSB::REG_DIST;
                break;
            default:
                break;
            }
            swInfo.minDist = m_depInfo.regDist;
            swInfo.distType = swsb_type;
        }
        SWSB::InstType inst_type = SWSB::InstType::OTHERS;
        if (m_opSpec->isSendOrSendsFamily())
            inst_type = SWSB::InstType::SEND;
        else if (m_opSpec->isMathSubFunc())
            inst_type = SWSB::InstType::MATH;
        if (!swInfo.verify(m_model.getSWSBEncodeMode(), inst_type))
            m_errorHandler.reportError(m_loc,
                "Invalid SWSB token and dist combination");
        inst->setSWSB(swInfo);

        m_pc += inst->hasInstOpt(InstOpt::COMPACTED) ? 8 : 16;

        // after any branching instruction or EOT, split the basic block
        // Also split when there is mov with label src
        if (inst->isBranching() || inst->hasInstOpt(InstOpt::EOT) ||
            inst->isMovWithLabel()) {
            BlockEnd(m_pc);
        }
    }


    void InstPredication(
        const Loc &loc,
        bool inv,
        const RegRef &flagReg,
        PredCtrl predCtrl)
    {
        m_predication.inverse = inv;
        m_predication.function = predCtrl;
        m_flagReg = flagReg;
    }


    void InstOp(const OpSpec *spec) {
        m_opSpec = spec;
    }


    // The absense of this implies branch control is either off or not
    // present (e.g. HSW or an instruction without that option).
    //
    // E.g.   if.b   (16)   64    80        // YES
    // E.g.   if     (16)   64    80        // NO
    //        while ...                     // NO
    void InstBrCtl(BranchCntrl bc) {
        m_brnchCtrl = bc;
    }


    void InstExecInfo(
        const Loc &execSizeLoc, ExecSize execSize,
        const Loc &execOffLoc, ChannelOffset execOff)
    {
        m_execSize = execSize;
        m_chOff = execOff;
    }


    void InstNoMask(const Loc &loc) {
        m_maskCtrl = MaskCtrl::NOMASK;
    }


    // The flag modifier (condition modifier)
    void InstFlagModifier(
        RegRef flagReg,
        FlagModifier flmodf)
    {
        m_flagModifier = flmodf;
        m_flagReg = flagReg;
    }


    /////////////////////////////////////////////
    // destination operand callbacks

    // (sat) applied to the destination operand
    void InstDstOpSaturate() {
        m_dstModifier = DstModifier::SAT;
    }

    // direct access
    //
    // e.g. r13.4<2>:t
    void InstDstOpRegDirect(
        const Loc &loc,
        const RegInfo &ri,
        RegRef reg,
        Region::Horz rgnHorz,
        Type ty)
    {
        InstDstOpRegDirect(loc,ri.regName,reg,rgnHorz,ty);
    }
    void InstDstOpRegDirect(
        const Loc &loc,
        RegName rn,
        int reg,
        Region::Horz rgnHorz,
        Type ty)
    {
        RegRef rr;
        rr.regNum = (uint8_t)reg;
        InstDstOpRegDirect(loc,rn,rr,rgnHorz,ty);
    }
    void InstDstOpRegDirect(
        const Loc &loc,
        RegName rn,
        RegRef reg,
        Region::Horz rgnHorz,
        Type ty)
    {
        m_dst.kind = Operand::Kind::DIRECT;
        m_dst.loc = loc;

        m_dst.regOpDstMod = m_dstModifier;
        m_dst.regOpName = rn;
        m_dst.regOpReg = reg;
        m_dst.regOpRgn.setDstHz(rgnHorz);
        m_dst.type = ty;
    }
    // math macro register access (implicit accumulator)
    //
    // e.g. r13.acc4:t
    void InstDstOpRegMathMacroExtReg(
        const Loc &loc,
        RegName rnm,
        int regNum,
        MathMacroExt mme,
        Region::Horz rgnH,
        Type ty)
    {
        m_dst.kind = Operand::Kind::MACRO;
        m_dst.loc = loc;

        m_dst.regOpDstMod = m_dstModifier;
        m_dst.regOpName = rnm;
        m_dst.regOpReg = RegRef(static_cast<uint8_t>(regNum), 0);
        m_dst.regOpReg.regNum = (uint8_t)regNum;
        m_dst.regOpRgn.setDstHz(rgnH);
        m_dst.regOpMathMacroExtReg = mme;
        m_dst.type = ty;
    }
    void InstDstOpRegMathMacroExtReg(
        const Loc &loc,
        const RegInfo &ri,
        RegName rnm,
        int regNum,
        MathMacroExt mme,
        Region::Horz rgnH,
        Type ty)
    {
        InstDstOpRegMathMacroExtReg(loc, ri.regName, regNum, mme, rgnH, ty);
    }

    // e.g. r[a0.4,16]<2>:t
    void InstDstOpRegIndirect(
        const Loc &loc,
        RegRef addrReg,
        int addrOff,
        Region::Horz rgnHorz,
        Type ty)
    {
        m_dst.kind = Operand::Kind::INDIRECT;
        m_dst.loc = loc;

        m_dst.regOpDstMod = m_dstModifier;
        m_dst.regOpName = RegName::GRF_R;
        m_dst.regOpReg = addrReg;
        m_dst.regOpIndOff = (uint16_t)addrOff;
        m_dst.regOpRgn.setDstHz(rgnHorz);
        m_dst.type = ty;
    }

    // a more generic setter
    void InstDstOp(const OperandInfo &opInfo) {
        m_dst = opInfo;
        validateOperandInfo(opInfo);
    }

    /////////////////////////////////////////////
    // source operand callbacks

    // Direct register source operand
    //
    // e.g. r13.4<2>:t
    void InstSrcOpRegDirect(
        int srcOpIx, // index of the current source operand
        const Loc &loc,
        RegName rnm, // the type of register
        int reg, // register/subregister
        Region rgn, // region parameters
        Type ty)
    {
        RegRef rr;
        rr.regNum = (uint8_t)reg;
        InstSrcOpRegDirect(srcOpIx, loc, SrcModifier::NONE, rnm, rr, rgn, ty);
    }
    void InstSrcOpRegDirect(
        int srcOpIx, // index of the current source operand
        const Loc &loc,
        SrcModifier srcMod, // source modifiers on this operand
        RegName rnm, // the type of register
        RegRef rr, // register/subregister
        Region rgn, // region parameters
        Type ty)
    {
        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::DIRECT;
        src.regOpSrcMod = srcMod;
        src.regOpName = rnm;
        src.regOpReg = rr;
        src.regOpRgn = rgn;
        src.type = ty;

        InstSrcOp(srcOpIx, src);
    }
    // math macro register access
    //
    // e.g. r13.acc4:t
    void InstSrcOpRegMathMacroExtReg(
        int srcOpIx, // index of the current source operand
        const Loc &loc,
        SrcModifier srcMod, // source modifiers on this operand
        RegName rnm, // the type of register
        int regNum,
        MathMacroExt MathMacroExt,
        Region rgn,
        Type ty)
    {
        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::MACRO;
        src.regOpSrcMod = srcMod;
        src.regOpName = rnm;
        src.regOpReg.regNum = (uint8_t)regNum;
        src.regOpReg.subRegNum = 0;
        src.regOpRgn = rgn;
        src.regOpMathMacroExtReg = MathMacroExt;
        src.type = ty;

        InstSrcOp(srcOpIx, src);
    }
    // parsed a source indirect operand
    //
    // e.g. "r[a0.4,16]<1,0>:f"
    void InstSrcOpRegIndirect(
        int srcOpIx, // index of the current source operand
        const Loc &loc,
        const SrcModifier &srcMod, // source modifiers on this operand
        RegRef addrReg,
        int addrOff, // e.g. 16 in r[a0.3,16] (0 if absent)
        Region rgn,
        Type ty)
    {
        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::INDIRECT;
        src.regOpSrcMod = srcMod;
        src.regOpName = RegName::GRF_R;
        src.regOpReg = addrReg;
        src.regOpIndOff = (uint16_t)addrOff;
        src.regOpRgn = rgn;
        src.type = ty;

        InstSrcOp(srcOpIx, src);
    }


    // Called on a source immediate operand.  Some immedidate operands
    // will not have explicit types.  E.g. send descriptors.
    //
    // e.g. 14:d
    //      0x21424
    void InstSrcOpImmValue(
        int srcOpIx,
        const Loc &loc,
        const ImmVal &val,
        Type ty)
    {
        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::IMMEDIATE;
        src.immValue = val;
        src.type = ty;

        InstSrcOp(srcOpIx, src);
    }


    // Called when an immediate label is encountered (e.g. on branches)
    void InstSrcOpImmLabel(
        int srcOpIx,
        const Loc &loc,
        const std::string &sym,
        Type type)
    {
        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::LABEL;
        src.immLabel = sym;
        src.type = type;

        InstSrcOp(srcOpIx, src);
    }


    // almost all cases
    void InstSrcOpImmLabelRelative(
      int srcOpIx,
      const Loc &loc,
      int64_t relPc,
      Type type)
    {
        // NOTE: even though jmpi is relative post-increment, remember
        // that IGA keeps the offsets normalized as pre-increment and in
        // bytes for uniformity (HSW had some QWord labels)
        // (after GEN12 jmpi is also pre-increment)

        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::LABEL;
        src.immValue = relPc;
        src.type = type;

        InstSrcOp(srcOpIx, src);
    }


    // calla directly calls this, everything else goes through relative
    // (see above)
    void InstSrcOpImmLabelAbsolute(
      int srcOpIx,
      const Loc &loc,
      int64_t absPc, // the actual PC relative to program start
      Type type)
    {
        OperandInfo src = m_srcs[srcOpIx]; // copy init values
        src.loc = loc;
        src.kind = Operand::Kind::LABEL;
        src.immValue = absPc;
        src.type = type;

        InstSrcOp(srcOpIx, src);
    }


    // a more generic setter
    void InstSrcOp(int srcOpIx, const OperandInfo &opInfo) {
        m_nSrcs = m_nSrcs < srcOpIx + 1 ? srcOpIx + 1 : m_nSrcs;

        validateOperandInfo(opInfo);
        m_srcs[srcOpIx] = opInfo;
    }

    void validateOperandInfo(const OperandInfo &opInfo) {
#ifdef _DEBUG
        // some sanity validation
        switch (opInfo.kind) {
        case Operand::Kind::DIRECT:
            break;
        case Operand::Kind::MACRO:
            break;
        case Operand::Kind::INDIRECT:
            break;
        case Operand::Kind::IMMEDIATE:
        case Operand::Kind::LABEL:
            break;
        default:
            IGA_ASSERT_FALSE("OperandInfo::kind: invalid value");
            break;
        }
#endif
    }

    // send descriptors
    // E.g. "send ... 0xC  a0.0"
    // (we translate  this to:  a0.0<0;1,0>:ud)
    void InstSendDescs(
        const Loc &locExDesc,
        const SendDescArg &exDesc,
        const Loc &locDesc,
        const SendDescArg &desc)
    {
        m_exDesc = exDesc;
        m_desc = desc;
    }


    /////////////////////////////////////////////
    // instruction option callbacks
    void InstOpts(const InstOptSet &instOpts) {
        m_instOpts = instOpts;
    }


    void setDepInfoSBidSrc(Loc loc, int32_t sbid) {
        if (sbid > (int32_t)m_model.getSWSBTokenNum())
            m_errorHandler.reportError(loc, "Invalid SWSB ID number");
        if (m_depInfo.anyBarrierSet())
            m_errorHandler.reportError(loc, "More than one SWSB barrier set");

        m_depInfo.memSBidSrc = sbid;
    }

    void setDepInfoSBidDst(Loc loc, int32_t sbid) {
        if (sbid > (int32_t)m_model.getSWSBTokenNum())
            m_errorHandler.reportError(loc, "Invalid SWSB ID number");
        if (m_depInfo.anyBarrierSet())
            m_errorHandler.reportError(loc, "More than one SWSB barrier set");

        m_depInfo.memSBidDst = sbid;
    }

    void setDepInfoSBidAlloc(Loc loc, int32_t sbid) {
        if (sbid > (int32_t)m_model.getSWSBTokenNum())
            m_errorHandler.reportError(loc, "Invalid SWSB ID number");
        if (m_depInfo.anyBarrierSet())
            m_errorHandler.reportError(loc, "More than one SWSB barrier set");

        m_depInfo.memSBidAlloc = sbid;
    }

    void setDepInfoDist(Loc loc, SWSBInfo::DIST_TYPE type, uint32_t dist) {
        if (dist > m_model.getSWSBMaxValidDistance())
            m_errorHandler.reportError(loc, "Invalid SWSB distance number");
        if (m_depInfo.distType != SWSBInfo::DIST_TYPE::NONE)
            m_errorHandler.reportError(loc, "More than one SWSB distance set");
        m_depInfo.distType = type;
        m_depInfo.regDist = dist;
    }

    ///////////////////////////////////////////////
    // for decoding from binary
    void InstSwsb(Loc loc, SWSB swsb) {
        m_depInfo = SWSBInfo(); // clobber old value
        // verify the given swsb
        SWSB::InstType inst_type = SWSB::InstType::OTHERS;
        if (m_opSpec->isSendOrSendsFamily())
            inst_type = SWSB::InstType::SEND;
        else if (m_opSpec->isMathSubFunc())
            inst_type = SWSB::InstType::MATH;
        if (!swsb.verify(m_model.getSWSBEncodeMode(), inst_type))
            m_errorHandler.reportError(loc, "invalid SWSB bits");


        switch(swsb.tokenType) {
        case SWSB::TokenType::NOTOKEN:
            break;
        case SWSB::TokenType::DST:
            m_depInfo.memSBidDst = (int32_t)swsb.sbid;
            break;
        case SWSB::TokenType::SRC:
            m_depInfo.memSBidSrc = (int32_t)swsb.sbid;
            break;
        case SWSB::TokenType::SET:
            m_depInfo.memSBidAlloc = (int32_t)swsb.sbid;
            break;
        }

        switch (swsb.distType) {
        case SWSB::DistType::NO_DIST:
            m_depInfo.distType = SWSBInfo::NONE;
            break;
        case SWSB::DistType::REG_DIST:
            m_depInfo.distType = SWSBInfo::REG_DIST;
            break;
        default:
            m_errorHandler.reportError(loc, "invalid SWSB bits");
        }
        if (swsb.distType != SWSB::DistType::NO_DIST)
            m_depInfo.regDist = swsb.minDist;
    }


    // sets Instruction::setComment()
    void InstComment(std::string comment)
    {
        m_comment = comment;
    }

}; // class ParseHandler

} // namespace

#endif //_IR_BUILDER_HANDLER_HPP_
