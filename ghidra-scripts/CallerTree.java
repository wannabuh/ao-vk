// Prints the caller tree (upwards) of functions matching a regex, until an exported function, a function
// with no callers, or the depth limit. Indirect callers (vtables, callbacks) are invisible to this, so a
// "(no callers)" root is often a virtual method; its vtable references are listed when found.
//
// Args: <out.txt> <depth> <regex>...
//@category randy-vk

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;
import java.util.regex.*;

public class CallerTree extends GhidraScript {

    PrintWriter out;
    int maxDepth;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        out = new PrintWriter(new FileWriter(args[0]));
        maxDepth = Integer.parseInt(args[1]);
        FunctionManager fm = currentProgram.getFunctionManager();
        for (int i = 2; i < args.length; i++) {
            Pattern re = Pattern.compile(args[i]);
            List<Function> all = new ArrayList<>();
            fm.getFunctions(true).forEach(all::add);
            fm.getExternalFunctions().forEach(all::add);       // imports, e.g. RANDY31.DLL functions
            for (Function f : all) {
                Function real = f.isThunk() && f.getThunkedFunction(true) != null ? f.getThunkedFunction(true) : f;
                if (!re.matcher(real.getName(true)).find()) continue;
                out.println("== callers of " + real.getName(true) + (f.isThunk() ? " (via thunk " + f.getEntryPoint() + ")" : ""));
                walk(f, 1, new HashSet<>());
            }
        }
        out.close();
        println("caller tree -> " + args[0]);
    }

    boolean isExported(Function f) {
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(f.getEntryPoint()))
            if (s.isExternalEntryPoint()) return true;
        return currentProgram.getSymbolTable().isExternalEntryPoint(f.getEntryPoint());
    }

    void walk(Function f, int depth, Set<Function> path) {
        Set<Function> callers = f.getCallingFunctions(monitor);
        String pad = "  ".repeat(depth);
        if (callers.isEmpty()) {
            StringBuilder data = new StringBuilder();
            for (Reference r : getReferencesTo(f.getEntryPoint()))
                if (r.getReferenceType().isData()) data.append(" ").append(r.getFromAddress());
            if (data.length() > 0) out.println(pad + "(no direct callers; data refs:" + data + ")");
            return;
        }
        for (Function c : callers) {
            String tag = isExported(c) ? "  [EXPORT]" : "";
            out.println(pad + c.getEntryPoint() + "  " + c.getName(true) + tag);
            boolean stopAtExport = System.getenv("CALLERTREE_THROUGH_EXPORTS") == null;
            if ((stopAtExport && !tag.isEmpty()) || depth >= maxDepth || !path.add(c)) continue;
            walk(c, depth + 1, path);
            path.remove(c);
        }
    }
}
