// For every call to a function (internal or imported) whose name matches a regex, prints the call's
// arguments, constants as hex and everything else as "?".
//
// Args: <out.tsv> <regex>...
// Output columns: module, callee, func_addr, call_addr, args (comma separated; thiscall `this` included)
//@category randy-vk

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;
import java.util.regex.*;

public class ConstArgs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        List<Pattern> pats = new ArrayList<>();
        for (int i = 1; i < args.length; i++) pats.add(Pattern.compile(args[i]));
        FunctionManager fm = currentProgram.getFunctionManager();

        // Callee set: matching internal functions, imports, and thunks pointing at either.
        Map<Function, String> callees = new HashMap<>();
        List<Function> all = new ArrayList<>();
        fm.getFunctions(true).forEach(all::add);
        fm.getExternalFunctions().forEach(all::add);
        for (Function f : all) {
            Function real = f.isThunk() && f.getThunkedFunction(true) != null ? f.getThunkedFunction(true) : f;
            for (Pattern p : pats)
                if (p.matcher(real.getName(true)).find()) callees.put(f, real.getName(true));
        }

        Set<Function> callers = new HashSet<>();
        for (Function c : callees.keySet()) callers.addAll(c.getCallingFunctions(monitor));
        // Imports are called through the IAT; getCallingFunctions misses those, so add reference sources.
        for (Function c : callees.keySet())
            for (Reference r : getReferencesTo(c.getEntryPoint())) {
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f != null) callers.add(f);
                for (Reference r2 : getReferencesTo(r.getFromAddress())) {      // IAT slot -> call sites
                    Function g = fm.getFunctionContaining(r2.getFromAddress());
                    if (g != null) callers.add(g);
                }
            }

        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        out.println("module\tcallee\tfunc_addr\tcall_addr\targs");
        DecompInterface ifc = new DecompInterface();
        ifc.toggleCCode(false);
        ifc.openProgram(currentProgram);
        int rows = 0;
        for (Function f : callers) {
            if (monitor.isCancelled()) break;
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            HighFunction hf = r.getHighFunction();
            if (hf == null) continue;
            Iterator<PcodeOpAST> ops = hf.getPcodeOps();
            while (ops.hasNext()) {
                PcodeOpAST op = ops.next();
                if (op.getOpcode() != PcodeOp.CALL && op.getOpcode() != PcodeOp.CALLIND) continue;
                String name = resolve(op, callees);
                if (name == null) continue;
                StringBuilder sb = new StringBuilder();
                for (int i = 1; i < op.getNumInputs(); i++) {
                    Varnode a = op.getInput(i);
                    sb.append(i > 1 ? "," : "").append(a.isConstant() ? "0x" + Long.toHexString(a.getOffset()) : "?");
                }
                out.printf("%s\t%s\t%s\t%s\t%s%n", currentProgram.getName(), name, f.getEntryPoint(),
                        op.getSeqnum().getTarget(), sb);
                rows++;
            }
        }
        ifc.dispose();
        out.close();
        println("callees: " + callees.size() + ", callers: " + callers.size() + ", calls: " + rows);
    }

    String resolve(PcodeOp op, Map<Function, String> callees) {
        if (op.getOpcode() == PcodeOp.CALL) {
            Function c = getFunctionAt(op.getInput(0).getAddress());
            if (c != null && callees.containsKey(c)) return callees.get(c);
        }
        // Indirect call through an IAT slot: the instruction references the external function.
        for (Reference r : getReferencesFrom(op.getSeqnum().getTarget())) {
            Function c = getFunctionAt(r.getToAddress());
            if (c != null && callees.containsKey(c)) return callees.get(c);
            for (Reference r2 : getReferencesFrom(r.getToAddress())) {
                Function d = getFunctionAt(r2.getToAddress());
                if (d != null && callees.containsKey(d)) return callees.get(d);
            }
        }
        return null;
    }
}
