// Lists COM-style virtual calls, obj->vtbl[n](obj, ...), with a short description of where obj comes from
// (e.g. "*(this+0x0)" or "*(*(&render_t::m_pcInstance)+0x0)") and every constant argument.
// The interface is not known to the script; map (object expression, vtable offset) to methods afterwards
// (tools/com_report.py does that for the DirectX 7 interfaces).
//
// Args: <out.tsv>
// Output columns: func_addr, func, call_addr, object, vtable_offset, const_args (index=value, ...)
//@category randy-vk

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;

public class ComCalls extends GhidraScript {

    @Override
    public void run() throws Exception {
        PrintWriter out = new PrintWriter(new FileWriter(getScriptArgs()[0]));
        out.println("func_addr\tfunc\tcall_addr\tobject\tvtable_offset\tconst_args");
        DecompInterface ifc = new DecompInterface();
        ifc.toggleCCode(false);
        ifc.openProgram(currentProgram);
        int funcs = 0, rows = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) break;
            if (f.isThunk() || f.isExternal() || !hasIndirectCall(f)) continue;
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            HighFunction hf = r.getHighFunction();
            if (hf == null) continue;
            funcs++;
            Iterator<PcodeOpAST> ops = hf.getPcodeOps();
            while (ops.hasNext()) {
                PcodeOpAST op = ops.next();
                if (op.getOpcode() != PcodeOp.CALLIND || op.getNumInputs() < 2) continue;
                // target = LOAD(vtbl + off); vtbl = LOAD(obj); first argument must be obj
                PcodeOp load = skipCasts(op.getInput(0)).getDef();
                if (load == null || load.getOpcode() != PcodeOp.LOAD) continue;
                Varnode slot = skipCasts(load.getInput(1));
                long off = 0;
                PcodeOp add = slot.getDef();
                Varnode vtbl = slot;
                if (add != null && (add.getOpcode() == PcodeOp.INT_ADD || add.getOpcode() == PcodeOp.PTRSUB)
                        && add.getInput(1).isConstant()) {
                    off = add.getInput(1).getOffset();
                    vtbl = add.getInput(0);
                }
                PcodeOp vload = skipCasts(vtbl).getDef();
                if (vload == null || vload.getOpcode() != PcodeOp.LOAD) continue;
                Varnode obj = skipCasts(vload.getInput(1));
                Varnode arg0 = skipCasts(op.getInput(1));
                if (!sameValue(obj, arg0)) continue;
                StringBuilder consts = new StringBuilder();
                for (int i = 2; i < op.getNumInputs(); i++) {
                    Varnode a = op.getInput(i);
                    if (a.isConstant()) consts.append(consts.length() > 0 ? "," : "").append(i - 2).append("=0x")
                            .append(Long.toHexString(a.getOffset()));
                }
                out.printf("%s\t%s\t%s\t%s\t0x%X\t%s%n", f.getEntryPoint(), f.getName(true),
                        op.getSeqnum().getTarget(), describe(obj, 0), off, consts);
                rows++;
            }
        }
        ifc.dispose();
        out.close();
        println("functions with COM calls: " + funcs + ", calls: " + rows);
    }

    boolean hasIndirectCall(Function f) {
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType().isCall() && ins.getFlowType().isComputed()) return true;
        }
        return false;
    }

    static Varnode skipCasts(Varnode v) {
        for (int i = 0; i < 8 && v != null; i++) {
            PcodeOp d = v.getDef();
            if (d == null || (d.getOpcode() != PcodeOp.CAST && d.getOpcode() != PcodeOp.COPY)) break;
            v = d.getInput(0);
        }
        return v;
    }

    static boolean sameValue(Varnode a, Varnode b) {
        if (a == b) return true;
        if (a.getHigh() != null && a.getHigh() == b.getHigh()) return true;
        PcodeOp da = a.getDef(), db = b.getDef();
        // Two separate loads of the same address (common after inlining): compare their address operands.
        if (da != null && db != null && da.getOpcode() == PcodeOp.LOAD && db.getOpcode() == PcodeOp.LOAD)
            return sameValue(skipCasts(da.getInput(1)), skipCasts(db.getInput(1)));
        if (da != null && db != null && da.getOpcode() == db.getOpcode()
                && (da.getOpcode() == PcodeOp.INT_ADD || da.getOpcode() == PcodeOp.PTRSUB)
                && da.getInput(1).isConstant() && db.getInput(1).isConstant()
                && da.getInput(1).getOffset() == db.getInput(1).getOffset())
            return sameValue(skipCasts(da.getInput(0)), skipCasts(db.getInput(0)));
        if (a.isAddress() && b.isAddress()) return a.getAddress().equals(b.getAddress());
        return false;
    }

    String describe(Varnode v, int depth) {
        v = skipCasts(v);
        if (depth > 5) return "?";
        if (v.isConstant()) return "0x" + Long.toHexString(v.getOffset());
        if (v.isAddress()) {
            Symbol s = getSymbolAt(v.getAddress());
            return "&" + (s != null ? s.getName(true) : v.getAddress().toString());
        }
        HighVariable h = v.getHigh();
        if (h instanceof HighParam hp) return hp.getSlot() == 0 ? "param0/this" : "param" + hp.getSlot();
        PcodeOp d = v.getDef();
        if (d == null) {
            if (v.isRegister()) {
                Register r = currentProgram.getRegister(v);
                return r != null && r.getName().equals("ECX") ? "this" : (r != null ? r.getName() : "reg");
            }
            return h != null && h.getName() != null ? h.getName() : "?";
        }
        switch (d.getOpcode()) {
            case PcodeOp.LOAD:
                return "*(" + describe(d.getInput(1), depth + 1) + ")";
            case PcodeOp.INT_ADD:
            case PcodeOp.PTRSUB:
                if (d.getInput(1).isConstant())
                    return describe(d.getInput(0), depth + 1) + "+0x" + Long.toHexString(d.getInput(1).getOffset());
                return describe(d.getInput(0), depth + 1) + "+" + describe(d.getInput(1), depth + 1);
            case PcodeOp.PTRADD:
                return describe(d.getInput(0), depth + 1) + "[" + describe(d.getInput(1), depth + 1) + "]";
            case PcodeOp.CALL: {
                Function c = getFunctionAt(d.getInput(0).getAddress());
                return (c != null ? c.getName(true) : "call") + "()";
            }
            case PcodeOp.MULTIEQUAL:
                return "phi(" + describe(d.getInput(0), depth + 1) + ")";
            default:
                return d.getMnemonic().toLowerCase() + "(...)";
        }
    }
}
