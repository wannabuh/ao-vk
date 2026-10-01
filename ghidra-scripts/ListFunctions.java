// Writes "entry_rva size name" for every function of the program (for bucketing profiler samples).
// Args: <out.txt>
//@category randy-vk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.*;

public class ListFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = currentProgram.getImageBase().getOffset();
        try (PrintWriter out = new PrintWriter(new FileWriter(getScriptArgs()[0]))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true))
                out.printf("%x %x %s%n", f.getEntryPoint().getOffset() - base, f.getBody().getNumAddresses(), f.getName(true));
        }
    }
}
