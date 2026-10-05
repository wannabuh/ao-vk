// The program's call graph for tools/port-status.py: one line per function,
// "<rva> <flags> c=<caller rva>,... v=<vtable rva>,... o=<owner rva>,...", flags: e = exported (an entry point),
// d = referenced from somewhere that can't be traced to a function (an unconditional root), '-' = neither. Callers
// are the functions containing a call to it; vtables (labels named "vftable") are those holding it; owners are the
// functions that take its address (code), or that reference the data holding it (up to four levels: an exception
// handler's unwind map -> FuncInfo -> the function).
// Then one line per vtable: "vtable <rva> f=<function rva>,... data=<0|1>": the functions referencing it (its
// constructors and destructors write it into objects) and whether anything else does (static objects).
//
// Args: <out.txt> [<export directory rva> <its size>]   (hex; references from it don't count as 'd': an export is
// flagged e, and tools/port-status.py decides whether anything imports it)
//@category randy-vk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Symbol;

import java.io.PrintWriter;
import java.util.TreeSet;

public class CallGraph extends GhidraScript {
    long base;

    // The vtable holding `at`: the nearest label at or before it is a "vftable" (vtables follow one another, each
    // after its own meta pointer label). -1 if none.
    long vtableAt(Address at) {
        var symbols = currentProgram.getSymbolTable();
        Address a = at;
        for (int i = 0; i < 0x200; ++i) {
            Symbol[] s = symbols.getSymbols(a);
            if (s.length > 0) {
                for (Symbol sym : s)
                    if (sym.getName().equals("vftable")) return a.getOffset() - base;
                return -1;
            }
            if (a.getOffset() - base < 4) return -1;
            a = a.subtract(4);
        }
        return -1;
    }

    long exportStart = -1, exportEnd = -1;

    // The functions that reference (through up to `depth` levels of data) the data item holding `at`; false if some
    // path ends nowhere (nothing references it, or too deep).
    boolean owners(Address at, int depth, TreeSet<Long> out) {
        if (depth > 4) return false;
        var functions = currentProgram.getFunctionManager();
        Data d = currentProgram.getListing().getDataContaining(at);
        Address start = d != null ? d.getMinAddress() : at;
        while (d != null && d.getParent() != null) d = d.getParent();
        if (d != null) start = d.getMinAddress();
        boolean any = false;
        for (Reference r : currentProgram.getReferenceManager().getReferencesTo(start)) {
            if (!r.getFromAddress().isMemoryAddress()) return false;
            long from = r.getFromAddress().getOffset() - base;
            if (from >= exportStart && from < exportEnd) continue;
            any = true;
            Function f = functions.getFunctionContaining(r.getFromAddress());
            if (f != null) out.add(f.getEntryPoint().getOffset() - base);
            else if (vtableAt(r.getFromAddress()) >= 0) return false;
            else if (!owners(r.getFromAddress(), depth + 1, out)) return false;
        }
        return any;
    }

    static String join(TreeSet<Long> set) {
        StringBuilder b = new StringBuilder();
        for (long v : set) {
            if (b.length() > 0) b.append(',');
            b.append(Long.toHexString(v));
        }
        return b.toString();
    }

    @Override
    public void run() throws Exception {
        base = currentProgram.getImageBase().getOffset();
        var functions = currentProgram.getFunctionManager();
        var symbols = currentProgram.getSymbolTable();
        String[] args = getScriptArgs();
        exportStart = args.length > 2 ? Long.parseLong(args[1], 16) : -1;
        exportEnd = args.length > 2 ? exportStart + Long.parseLong(args[2], 16) : -1;
        try (PrintWriter w = new PrintWriter(args[0])) {
            for (Function f : functions.getFunctions(true)) {
                boolean exported = symbols.isExternalEntryPoint(f.getEntryPoint());
                boolean data = false;
                TreeSet<Long> callers = new TreeSet<>(), vtables = new TreeSet<>(), owned = new TreeSet<>();
                for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
                    RefType t = r.getReferenceType();
                    if (exportStart >= 0 && !r.getFromAddress().isMemoryAddress()) continue;   // "entry point"
                    long at = r.getFromAddress().getOffset() - base;
                    if (at >= exportStart && at < exportEnd) continue;   // the export table
                    Function from = functions.getFunctionContaining(r.getFromAddress());
                    if ((t.isCall() || t.isJump()) && from != null) {
                        if (!from.equals(f)) callers.add(from.getEntryPoint().getOffset() - base);
                    } else if (t.isData() || t.isIndirect() || t.isRead() || from == null) {
                        if (from != null) {                          // its address taken by code
                            if (!from.equals(f)) owned.add(from.getEntryPoint().getOffset() - base);
                            continue;
                        }
                        long vt = r.getFromAddress().isMemoryAddress() ? vtableAt(r.getFromAddress()) : -1;
                        if (vt >= 0) vtables.add(vt);
                        else if (!owners(r.getFromAddress(), 1, owned)) data = true;
                    }
                }
                String flags = (exported ? "e" : "") + (data ? "d" : "");
                w.println(Long.toHexString(f.getEntryPoint().getOffset() - base) + " " + (flags.isEmpty() ? "-" : flags)
                          + " c=" + join(callers) + " v=" + join(vtables) + " o=" + join(owned));
            }
            for (Symbol s : symbols.getAllSymbols(true)) {
                if (!s.getName().equals("vftable")) continue;
                TreeSet<Long> users = new TreeSet<>();
                boolean data = false;
                for (Reference r : currentProgram.getReferenceManager().getReferencesTo(s.getAddress())) {
                    if (!r.getFromAddress().isMemoryAddress()) continue;
                    Function from = functions.getFunctionContaining(r.getFromAddress());
                    if (from != null) users.add(from.getEntryPoint().getOffset() - base);
                    else data = true;
                }
                w.println("vtable " + Long.toHexString(s.getAddress().getOffset() - base) + " f=" + join(users)
                          + " data=" + (data ? 1 : 0));
            }
        }
        println("call graph written");
    }
}
