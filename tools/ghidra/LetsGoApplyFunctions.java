// Seeds Ghidra with the function database before auto-analysis: every entry
// in data/main_functions.csv (from .eh_frame, exports and call targets) becomes
// a function, with its exported name where there is one. PLT stubs are left to
// the ELF loader, which turns them into thunks to the imported symbols.
//
// Headless: -preScript LetsGoApplyFunctions.java <path to main_functions.csv>
// @category LetsGo
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class LetsGoApplyFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: LetsGoApplyFunctions.java <main_functions.csv>");
            return;
        }
        List<String> lines = Files.readAllLines(Paths.get(args[0]));
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        FunctionManager fm = currentProgram.getFunctionManager();
        int created = 0, existing = 0, failed = 0;
        for (int i = 1; i < lines.size(); i++) {
            String[] f = lines.get(i).split(",", -1);
            if (f.length < 4 || f[3].equals("plt")) {
                continue;
            }
            Address a = space.getAddress(Long.parseUnsignedLong(f[0].substring(2), 16));
            if (fm.getFunctionAt(a) != null) {
                existing++;
                continue;
            }
            disassemble(a);
            Function fn = createFunction(a, f[2].isEmpty() ? null : f[2]);
            if (fn == null) {
                failed++;
            } else {
                created++;
            }
            if (i % 10000 == 0) {
                monitor.checkCancelled();
                println(String.format("  %d/%d", i, lines.size() - 1));
            }
        }
        println(String.format("LetsGo: %d functions created, %d already present, %d failed", created, existing, failed));
    }
}
