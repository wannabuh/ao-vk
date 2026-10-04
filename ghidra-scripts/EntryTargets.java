// Functions whose first 5 bytes are a jump / branch target (e.g. a loop starting at the entry), where a 5-byte jump
// patched over the entry (proxy/native Replace) would break the code that branches there. One line per function:
// "<rva hex> <name> <referenced offset>". Writes nothing else; an empty file means every entry is safe.
//
// Args: <out.txt>
//@category randy-vk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;

public class EntryTargets extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        long base = currentProgram.getImageBase().getOffset();
        int count = 0;
        try (PrintWriter w = new PrintWriter(out)) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                Address entry = f.getEntryPoint();
                for (int i = 1; i < 5; ++i) {
                    Address a = entry.add(i);
                    boolean hit = false;
                    for (Reference r : currentProgram.getReferenceManager().getReferencesTo(a)) {
                        if (r.getReferenceType().isFlow()) {
                            hit = true;
                            break;
                        }
                    }
                    if (hit) {
                        w.printf("%x %s %d%n", entry.getOffset() - base, f.getName(true), i);
                        ++count;
                        break;
                    }
                }
            }
        }
        println(count + " functions with a branch into their first 5 bytes -> " + out);
    }
}
