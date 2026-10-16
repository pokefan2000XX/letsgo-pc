// Writes Ghidra's decompiled C for the given functions to <outdir>/<address>.c.
// The output is derived from the game's code, so keep it under orig/.
//
// Headless (after analysis):
//   analyzeHeadless orig/ghidra letsgo -process main.elf -noanalysis -readOnly
//     -scriptPath tools/ghidra -postScript LetsGoExportDecomp.java <outdir> 0x7100004110 [...]
// @category LetsGo
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Paths;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;

public class LetsGoExportDecomp extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: LetsGoExportDecomp.java <outdir> <address> [<address>...]");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();
        int ok = 0;
        for (int i = 1; i < args.length; i++) {
            Address a = space.getAddress(Long.parseUnsignedLong(args[i].replaceFirst("^0x", ""), 16));
            Function fn = getFunctionContaining(a);
            if (fn == null) {
                printerr("no function at " + a);
                continue;
            }
            DecompileResults r = di.decompileFunction(fn, 300, monitor);
            if (!r.decompileCompleted()) {
                printerr("decompile failed at " + a + ": " + r.getErrorMessage());
                continue;
            }
            String c = "// " + fn.getName() + " @ " + fn.getEntryPoint() + "\n" + r.getDecompiledFunction().getC();
            Files.writeString(Paths.get(outDir.getPath(), fn.getEntryPoint().toString() + ".c"), c);
            ok++;
        }
        di.dispose();
        println(String.format("LetsGo: decompiled %d of %d functions into %s", ok, args.length - 1, outDir));
    }
}
