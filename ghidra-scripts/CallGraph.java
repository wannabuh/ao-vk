// The program's call graph for tools/port-status.py: one line per function,
// "<rva> <flags> <caller rva>,<caller rva>,...", flags: e = exported (an entry point), d = referenced other than by a
// call (vtables, function pointers, data), '-' = neither. Callers are the functions containing a call to it.
//
// Args: <out.txt>
//@category randy-vk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;

import java.io.PrintWriter;
import java.util.TreeSet;

public class CallGraph extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        var functions = currentProgram.getFunctionManager();
        var symbols = currentProgram.getSymbolTable();
        try (PrintWriter w = new PrintWriter(getScriptArgs()[0])) {
            for (Function f : functions.getFunctions(true)) {
                boolean exported = symbols.isExternalEntryPoint(f.getEntryPoint());
                boolean data = false;
                TreeSet<Long> callers = new TreeSet<>();
                for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
                    RefType t = r.getReferenceType();
                    Function from = functions.getFunctionContaining(r.getFromAddress());
                    if ((t.isCall() || t.isJump()) && from != null) {
                        if (!from.equals(f)) callers.add(from.getEntryPoint().getOffset() - base);
                    } else if (t.isData() || t.isIndirect() || t.isRead() || from == null) {
                        data = true;
                    }
                }
                StringBuilder line = new StringBuilder();
                line.append(Long.toHexString(f.getEntryPoint().getOffset() - base)).append(' ');
                String flags = (exported ? "e" : "") + (data ? "d" : "");
                line.append(flags.isEmpty() ? "-" : flags).append(' ');
                boolean first = true;
                for (long c : callers) {
                    if (!first) line.append(',');
                    line.append(Long.toHexString(c));
                    first = false;
                }
                w.println(line);
            }
        }
        println("call graph written");
    }
}
