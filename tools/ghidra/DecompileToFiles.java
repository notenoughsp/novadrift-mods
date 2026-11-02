// Decompiles functions of the current program into <out dir>/0x<address>.c, in parallel.
// Headless usage (no re-analysis of an already analyzed project):
//   analyzeHeadless <project dir> <project name> -process NovaDrift.exe -noanalysis -readOnly
//     -scriptPath tools/ghidra -postScript DecompileToFiles.java <out dir> <timeout s> <address file|address>...
// An address file has one address per line (0x140123450), e.g. column 2 of missing.tsv.
// @category NovaDrift

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.parallel.DecompileConfigurer;
import ghidra.app.decompiler.parallel.DecompilerCallback;
import ghidra.app.decompiler.parallel.ParallelDecompiler;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.util.task.TaskMonitor;

public class DecompileToFiles extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 3) {
            println("args: <out dir> <timeout s> <address file|address>...");
            return;
        }
        File out = new File(args[0]);
        out.mkdirs();
        int timeout = Integer.parseInt(args[1]);

        List<Function> functions = new ArrayList<>();
        for (int i = 2; i < args.length; i++) {
            List<String> addresses = new File(args[i]).isFile() ? Files.readAllLines(Paths.get(args[i])) : List.of(args[i]);
            for (String s : addresses) {
                s = s.trim();
                if (!s.matches("(0x)?[0-9a-fA-F]+")) continue;  // skips headers
                File target = new File(out, "0x" + s.replaceFirst("^0x", "").toLowerCase() + ".c");
                if (target.exists()) continue;  // already done (resume)
                Function f = getFunctionAt(toAddr(s));
                if (f != null) functions.add(f);
                else println("no function at " + s);
            }
        }
        println("decompiling " + functions.size() + " functions");

        DecompileConfigurer configurer = new DecompileConfigurer() {
            @Override
            public void configure(DecompInterface decompiler) {
                decompiler.setOptions(new DecompileOptions());
                decompiler.toggleCCode(true);
                decompiler.toggleSyntaxTree(false);
                decompiler.setSimplificationStyle("decompile");
            }
        };
        DecompilerCallback<Void> callback = new DecompilerCallback<Void>(currentProgram, configurer) {
            @Override
            public Void process(DecompileResults results, TaskMonitor m) throws Exception {
                Function f = results.getFunction();
                String address = "0x" + f.getEntryPoint().toString();
                if (results.decompileCompleted()) {
                    String header = "// name: " + f.getName() + "\n// address: " + address + "\n";
                    Files.writeString(new File(out, address + ".c").toPath(), header + results.getDecompiledFunction().getC());
                } else {
                    println("failed " + address + ": " + results.getErrorMessage());
                }
                return null;
            }
        };
        callback.setTimeout(timeout);
        try {
            ParallelDecompiler.decompileFunctions(callback, functions, monitor);
        } finally {
            callback.dispose();
        }
        println("done");
    }
}
