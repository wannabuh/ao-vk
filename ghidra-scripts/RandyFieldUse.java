// Finds how a module that imports randy31.dll touches Randy objects without going through exports:
// direct field loads/stores, virtual calls (vtable slot), vtable writes (subclass constructors) and
// allocation sizes (operator new followed by an imported constructor).
//
// A varnode is treated as "pointer to Randy class C" when it is
//   - the `this` argument of an imported __thiscall member of C,
//   - the return value of an import whose demangled return type is C* / C&,
//   - an argument passed where the import's demangled parameter type is C* / C&.
// Tracking is per function (no interprocedural propagation), so results are a lower bound.
//
// Headless: analyzeHeadless <proj> AO -process DisplaySystem.dll -noanalysis \
//           -scriptPath ghidra-scripts -postScript RandyFieldUse.java <out.tsv>
//@category randy-vk

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.demangler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.symbol.*;

import java.io.*;
import java.util.*;
import java.util.regex.*;

public class RandyFieldUse extends GhidraScript {

    static final String LIB = "randy31.dll";
    static final Pattern CLASS_PTR = Pattern.compile("^(?:const )?(?:class|struct) (\\w+)(?: const)? ?[*&]");

    static class Import {
        String mangled, demangled, cls;     // cls = owning class for members, null for free functions
        boolean thiscall, ctor;
        String retClass;                    // class of returned pointer/reference, or null
        List<String> paramClasses = new ArrayList<>();
    }

    Map<Address, Import> importByExtAddr = new HashMap<>();
    Set<String> randyClasses = new TreeSet<>();
    PrintWriter out;
    int rows;

    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs().length > 0 ? getScriptArgs()[0] : "field_uses.tsv";
        collectImports();
        println("randy imports: " + importByExtAddr.size() + ", classes: " + randyClasses.size());

        out = new PrintWriter(new FileWriter(outPath));
        out.println("module\tclass\tkind\toffset\tsize\tsource\tfunc_addr\tfunc");

        DecompInterface ifc = new DecompInterface();
        ifc.toggleCCode(false);
        ifc.openProgram(currentProgram);
        int n = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (monitor.isCancelled()) break;
            if (f.isThunk() || f.isExternal()) continue;
            if (!callsRandy(f)) continue;
            DecompileResults r = ifc.decompileFunction(f, 60, monitor);
            HighFunction hf = r.getHighFunction();
            if (hf == null) { println("decompile failed: " + f.getName() + " " + r.getErrorMessage()); continue; }
            analyze(f, hf);
            n++;
        }
        ifc.dispose();
        out.close();
        println("functions analyzed: " + n + ", rows: " + rows + " -> " + outPath);
    }

    // ---- imports -------------------------------------------------------------------------------

    void collectImports() {
        ExternalManager em = currentProgram.getExternalManager();
        SymbolTable st = currentProgram.getSymbolTable();
        // Demangled imports live in class namespaces below the library, so walk every external symbol.
        for (Symbol sym : st.getExternalSymbols()) {
            ExternalLocation loc = em.getExternalLocation(sym);
            if (loc == null || loc.getLibraryName() == null || !loc.getLibraryName().equalsIgnoreCase(LIB)) continue;
            String mangled = loc.getOriginalImportedName() != null ? loc.getOriginalImportedName() : loc.getLabel();
            Import imp = parse(mangled);
            if (imp != null) importByExtAddr.put(loc.getExternalSpaceAddress(), imp);
        }
    }

    Import parse(String mangled) {
        Import imp = new Import();
        imp.mangled = mangled;
        DemangledObject d;
        try {
            List<DemangledObject> all = DemanglerUtil.demangle(currentProgram, mangled, null);
            d = all.isEmpty() ? null : all.get(0);
        } catch (Exception e) { d = null; }
        if (d == null) { imp.demangled = mangled; return imp; }
        imp.demangled = d.getSignature(false);
        Demangled ns = d.getNamespace();
        if (ns != null) { imp.cls = ns.getName(); randyClasses.add(imp.cls); }
        if (d instanceof DemangledFunction df) {
            imp.thiscall = "__thiscall".equals(df.getCallingConvention());
            imp.ctor = imp.cls != null && imp.cls.equals(df.getName());
            if (df.getReturnType() != null) imp.retClass = classOf(df.getReturnType().getSignature());
            for (DemangledParameter p : df.getParameters())
                imp.paramClasses.add(classOf(p.getType().getSignature()));
        }
        return imp;
    }

    static String classOf(String type) {
        Matcher m = CLASS_PTR.matcher(type.trim());
        return m.find() ? m.group(1) : null;
    }

    Import importAt(Address instr) {
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (Reference ref : rm.getReferencesFrom(instr)) {
            Import imp = resolve(ref.getToAddress(), rm, 0);
            if (imp != null) return imp;
        }
        return null;
    }

    Import resolve(Address a, ReferenceManager rm, int depth) {
        if (a.isExternalAddress()) return importByExtAddr.get(a);
        if (depth > 2) return null;
        Function f = getFunctionAt(a);
        if (f != null && f.isThunk()) {
            Function t = f.getThunkedFunction(true);
            if (t != null && t.isExternal()) return importByExtAddr.get(t.getEntryPoint());
        }
        for (Reference r : rm.getReferencesFrom(a)) {           // IAT slot -> external
            Import imp = resolve(r.getToAddress(), rm, depth + 1);
            if (imp != null) return imp;
        }
        return null;
    }

    boolean callsRandy(Function f) {
        InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (ins.getFlowType().isCall() && importAt(ins.getAddress()) != null) return true;
        }
        return false;
    }

    // ---- per-function analysis -----------------------------------------------------------------

    void analyze(Function f, HighFunction hf) {
        Map<HighVariable, String> typed = new HashMap<>();
        Map<HighVariable, String> why = new HashMap<>();
        List<PcodeOpAST> calls = new ArrayList<>();
        Iterator<PcodeOpAST> ops = hf.getPcodeOps();
        while (ops.hasNext()) {
            PcodeOpAST op = ops.next();
            int oc = op.getOpcode();
            if (oc != PcodeOp.CALL && oc != PcodeOp.CALLIND) continue;
            calls.add(op);
            Import imp = importAt(op.getSeqnum().getTarget());
            if (imp == null) continue;
            int argBase = 1;
            if (imp.thiscall && imp.cls != null && op.getNumInputs() > 1) {
                mark(typed, why, op.getInput(1), imp.cls, "this:" + imp.demangled);
                argBase = 2;
                if (imp.ctor) recordNew(f, op.getInput(1), imp);
            }
            if (imp.retClass != null && randyClasses.contains(imp.retClass) && op.getOutput() != null)
                mark(typed, why, op.getOutput(), imp.retClass, "ret:" + imp.demangled);
            for (int i = 0; i < imp.paramClasses.size(); i++) {
                String c = imp.paramClasses.get(i);
                int in = argBase + i;
                if (c != null && randyClasses.contains(c) && in < op.getNumInputs())
                    mark(typed, why, op.getInput(in), c, "arg:" + imp.demangled);
            }
        }
        for (Map.Entry<HighVariable, String> e : typed.entrySet())
            for (Varnode vn : e.getKey().getInstances())
                followPointer(f, vn, e.getValue(), why.get(e.getKey()), 0, new HashSet<>());
    }

    void mark(Map<HighVariable, String> typed, Map<HighVariable, String> why, Varnode vn, String cls, String src) {
        if (vn == null || vn.isConstant()) return;
        HighVariable hv = vn.getHigh();
        if (hv == null) return;
        typed.putIfAbsent(hv, cls);
        why.putIfAbsent(hv, src);
    }

    // vn points at the start of a `cls` object plus `base` bytes.
    void followPointer(Function f, Varnode vn, String cls, String src, long base, Set<Varnode> seen) {
        if (vn == null || !seen.add(vn)) return;
        Iterator<PcodeOp> it = vn.getDescendants();
        while (it.hasNext()) {
            PcodeOp d = it.next();
            switch (d.getOpcode()) {
                case PcodeOp.COPY, PcodeOp.CAST, PcodeOp.MULTIEQUAL ->
                    followPointer(f, d.getOutput(), cls, src, base, seen);
                case PcodeOp.INT_ADD, PcodeOp.PTRSUB -> {
                    Varnode other = d.getInput(0) == vn ? d.getInput(1) : d.getInput(0);
                    if (other.isConstant())
                        followPointer(f, d.getOutput(), cls, src, base + signed(other), seen);
                }
                case PcodeOp.PTRADD -> {
                    if (d.getInput(0) == vn && d.getInput(1).isConstant() && d.getInput(2).isConstant())
                        followPointer(f, d.getOutput(), cls, src,
                                base + signed(d.getInput(1)) * d.getInput(2).getOffset(), seen);
                }
                case PcodeOp.LOAD -> {
                    if (d.getInput(1) != vn) break;
                    emit(f, cls, base == 0 ? "load_vptr?" : "load", base, d.getOutput().getSize(), src);
                    if (base == 0 && d.getOutput().getSize() == 4)
                        followVtable(f, d.getOutput(), cls, src, 0, new HashSet<>());
                }
                case PcodeOp.STORE -> {
                    if (d.getInput(1) == vn)
                        emit(f, cls, base == 0 ? "store_vptr?" : "store", base, d.getInput(2).getSize(), src);
                }
                case PcodeOp.CALL, PcodeOp.CALLIND -> {
                    if (base != 0) {
                        for (int i = 1; i < d.getNumInputs(); i++)
                            if (d.getInput(i) == vn) emit(f, cls, "addr_passed", base, 0, src);
                    }
                }
                default -> { }
            }
        }
    }

    // vn holds a vtable pointer of `cls` plus `base` bytes.
    void followVtable(Function f, Varnode vn, String cls, String src, long base, Set<Varnode> seen) {
        if (vn == null || !seen.add(vn)) return;
        Iterator<PcodeOp> it = vn.getDescendants();
        while (it.hasNext()) {
            PcodeOp d = it.next();
            switch (d.getOpcode()) {
                case PcodeOp.COPY, PcodeOp.CAST, PcodeOp.MULTIEQUAL ->
                    followVtable(f, d.getOutput(), cls, src, base, seen);
                case PcodeOp.INT_ADD, PcodeOp.PTRSUB -> {
                    Varnode other = d.getInput(0) == vn ? d.getInput(1) : d.getInput(0);
                    if (other.isConstant()) followVtable(f, d.getOutput(), cls, src, base + signed(other), seen);
                }
                case PcodeOp.LOAD -> {
                    if (d.getInput(1) != vn) break;
                    Varnode fn = d.getOutput();
                    Iterator<PcodeOp> uses = fn.getDescendants();
                    boolean called = false;
                    while (uses.hasNext()) {
                        PcodeOp u = uses.next();
                        if (u.getOpcode() == PcodeOp.CALLIND && u.getInput(0) == fn) called = true;
                    }
                    emit(f, cls, called ? "vcall" : "vtable_load", base, 4, src);
                }
                default -> { }
            }
        }
    }

    void recordNew(Function f, Varnode thisVn, Import ctor) {
        PcodeOp def = thisVn.getDef();
        for (int hops = 0; def != null && hops < 4 && (def.getOpcode() == PcodeOp.COPY
                || def.getOpcode() == PcodeOp.CAST || def.getOpcode() == PcodeOp.MULTIEQUAL); hops++)
            def = def.getInput(0).getDef();
        if (def == null || def.getOpcode() != PcodeOp.CALL || def.getNumInputs() < 2) return;
        Function callee = getFunctionAt(def.getInput(0).getAddress());
        String name = callee == null ? "" : callee.getName();
        if (callee != null && callee.isThunk() && callee.getThunkedFunction(true) != null)
            name = callee.getThunkedFunction(true).getName();
        if (!(name.contains("operator_new") || name.equals("??2@YAPAXI@Z") || name.contains("operator new")))
            return;
        Varnode size = def.getInput(1);
        if (size.isConstant()) emit(f, ctor.cls, "new_size", size.getOffset(), 0, "ctor:" + ctor.demangled);
    }

    static long signed(Varnode c) {
        long v = c.getOffset();
        int bits = c.getSize() * 8;
        if (bits < 64 && (v & (1L << (bits - 1))) != 0) v -= (1L << bits);
        return v;
    }

    void emit(Function f, String cls, String kind, long off, int size, String src) {
        out.printf("%s\t%s\t%s\t0x%X\t%d\t%s\t%s\t%s%n", currentProgram.getName(), cls, kind, off, size,
                src.replace('\t', ' '), f.getEntryPoint(), f.getName(true));
        rows++;
    }
}
