// Prints every reference to an address, with the symbol at or before the referring address.
// Args: <hex addr>...
//@category randy-vk
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.symbol.*;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            Address to = toAddr(a);
            for (Reference r : getReferencesTo(to)) {
                Address from = r.getFromAddress();
                Symbol s = getSymbolBefore(from);
                println(a + " <- " + from + " " + r.getReferenceType() + " near " + (s == null ? "?" : s.getName(true) + "@" + s.getAddress()));
            }
        }
    }
}
