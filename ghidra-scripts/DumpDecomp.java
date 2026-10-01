// Decompiles selected functions (and optionally their callees inside the same module) to one text file.
//
// Args: <out.c> <depth> <target>...
//   target = hex address (0x1000abcd), "strings:<regex>" (functions referencing a matching string), "calls:<regex>" (every caller of a matching function or import), "vtable:<hex addr>:<count>" (every slot of a vtable), or a
//            regex matched against the function's full name (namespace::name, as Ghidra shows it)
//   depth  = how many levels of internal callees to include (0 = only the targets)
//
// Headless: analyzeHeadless <proj> AO -process randy31.dll -noanalysis -readOnly \
//           -scriptPath ghidra-scripts -postScript DumpDecomp.java out.c 1 'Randy_t::Flip'
//@category randy-vk

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;
import java.util.regex.*;

public class DumpDecomp extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args[0];
        int depth = Integer.parseInt(args[1]);

        List<Function> roots = new ArrayList<>();
        Map<Function, String> notes = new HashMap<>();
        FunctionManager fm = currentProgram.getFunctionManager();
        for (int i = 2; i < args.length; i++) {
            String t = args[i];
            if (t.startsWith("vtable:")) {
                String[] p = t.split(":");
                Address vt = toAddr(Long.parseLong(p[1].replace("0x", ""), 16));
                int n = Integer.parseInt(p[2]);
                for (int s = 0; s < n; s++) {
                    Address fa = toAddr(getInt(vt.add(4L * s)) & 0xffffffffL);
                    var blk = currentProgram.getMemory().getBlock(fa);
                    if (blk == null || !blk.isExecute()) break;      // end of the vtable
                    Function f = fm.getFunctionAt(fa);
                    if (f == null) f = createFunction(fa, null);
                    if (f != null) { roots.add(f); notes.merge(f, "vtable " + vt + " slot " + s + " (+0x" + Integer.toHexString(4 * s) + ")", (a, b) -> a + "; " + b); }
                }
            } else if (t.startsWith("calls:")) {               // every function calling a matching function/import
                Pattern re = Pattern.compile(t.substring(6));
                for (Function f : fm.getFunctions(true))
                    for (Function c : f.getCalledFunctions(monitor)) {
                        Function real = c.isThunk() && c.getThunkedFunction(true) != null ? c.getThunkedFunction(true) : c;
                        if (re.matcher(real.getName(true)).find()) { roots.add(f); notes.merge(f, "calls " + real.getName(true), (a, b) -> a.contains(b) ? a : a + "; " + b); break; }
                    }
            } else if (t.startsWith("strings:")) {             // every function referencing a matching string
                Pattern re = Pattern.compile(t.substring(8));
                for (Data d : currentProgram.getListing().getDefinedData(true)) {
                    if (!d.hasStringValue() || !re.matcher(String.valueOf(d.getValue())).find()) continue;
                    for (Reference r : getReferencesTo(d.getAddress())) {
                        Function f = fm.getFunctionContaining(r.getFromAddress());
                        if (f != null && !roots.contains(f)) roots.add(f);
                    }
                }
            } else if (t.startsWith("0x")) {
                Function f = fm.getFunctionContaining(toAddr(Long.parseLong(t.substring(2), 16)));
                if (f != null) roots.add(f);
            } else {
                Pattern re = Pattern.compile(t);
                for (Function f : fm.getFunctions(true))
                    if (re.matcher(f.getName(true)).find()) roots.add(f);
            }
        }

        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        Set<Function> done = new LinkedHashSet<>();
        Deque<Map.Entry<Function, Integer>> queue = new ArrayDeque<>();
        for (Function f : roots) queue.add(Map.entry(f, 0));
        try (PrintWriter out = new PrintWriter(new FileWriter(outPath))) {
            while (!queue.isEmpty() && !monitor.isCancelled()) {
                var e = queue.poll();
                Function f = e.getKey();
                if (f.isThunk() || f.isExternal() || !done.add(f)) continue;
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                out.println("// ==== " + f.getEntryPoint() + "  " + f.getName(true)
                        + (notes.containsKey(f) ? "   [" + notes.get(f) + "]" : "")
                        + (e.getValue() > 0 ? "   (callee, depth " + e.getValue() + ")" : ""));
                if (r.getDecompiledFunction() != null) out.println(r.getDecompiledFunction().getC());
                else out.println("// decompile failed: " + r.getErrorMessage());
                if (e.getValue() < depth)
                    for (Function c : f.getCalledFunctions(monitor))
                        if (!c.isExternal() && !c.isThunk()) queue.add(Map.entry(c, e.getValue() + 1));
            }
        }
        ifc.dispose();
        println("dumped " + done.size() + " functions -> " + outPath);
    }
}
