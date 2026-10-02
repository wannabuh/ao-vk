// Prints "address namespace::name" for every symbol whose qualified name matches a regex.
// Args: <regex>
//@category randy-vk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;

public class FindSymbols extends GhidraScript {
    @Override
    public void run() throws Exception {
        String re = getScriptArgs()[0];
        for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
            String q = s.getName(true);
            if (q.matches(re))
                println(s.getAddress() + " " + q);
        }
    }
}
