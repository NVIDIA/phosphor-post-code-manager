# Branch coverage limitations: phosphor-post-code-manager

This document records branch coverage after UT-only improvements and documents remaining limitations (no production code changes). Coverage measured with gcovr, repo-only (src/ + inc/), after running unit tests with `b_coverage=true`. Numbers below reflect the latest run unless otherwise noted.

**Overall (repo-only, main.cpp excluded):** Lines 87.3% (302/346), Functions 94.1% (32/34), Branches 84.6% (296/350), Decisions 84.0% (68/81).


## How to improve branch coverage (no production code changes)

1. **gcovr config**
   - **Exclude `main.cpp`:** Already excluded in `gcovr.cfg` (entry point not in UT scope). This removes 30 branches from the report and raises the reported branch %.
   - **Exclude compiler “noise”:** `exclude-throw-branches = yes` and `exclude-unreachable-branches = yes` in `gcovr.cfg` reduce *some* compiler-generated branches (see below).

2. **More / better unit tests (no prod changes)**
   - **post_code.cpp:** Add tests for `findWithMask` (mask vs exact match), handler secondary optional, and edge cases in `getPostCodes` / `savePostCodes`. Add tests that force cereal/filesystem errors (e.g. bad stream, read-only FS) to hit remaining serialize/deserialize catch branches.
   - **post_code.hpp:** Use `PostCode`, `PostCodeHandler`, `PostCodeEvent`, and optionals in more combinations (different template instantiations and branch paths) so inline/operator branches get covered.
   - **nvidia_post_code_handler.cpp:** The `logNvidiaPostCode` catch block is hard to hit without D-Bus throwing; covering it would require an injectable bus or test double (test-only wiring, not production code change).

3. **What won’t improve without prod or tooling changes**
   - Compiler-generated exception arcs and destructor/move/copy branches (only partly filtered by gcovr).
   - main.cpp (excluded from report; use integration/system tests if you need coverage there).

### Why gcovr options don’t remove all “compiler-generated / inline” branches

The doc lists **compiler-generated / inline** and **operators, destructors, inline accessors** as limitations. The options in `gcovr.cfg` only cover a **narrow subset** of that:

| Option | What it actually excludes | What it does *not* exclude |
|--------|---------------------------|-----------------------------|
| **exclude-throw-branches** | Branches that GCC marks as *exception-only*: e.g. the arc from a call site “normal return” vs “go to exception handler”. | Inline/template code in headers, destructor/operator branches that aren’t exception arcs, branches *inside* your catch blocks, other generated control flow. |
| **exclude-unreachable-branches** | Branches on *lines* with no “useful” source (empty lines, lines with only `{}`). | Unreachable or “dead” code on lines that also have real code; compiler-generated branches attributed to normal source lines. |

So the remaining ~54 uncovered branches (350 total − 296 covered) include:

- **Header (inc/post_code.hpp):** Operators, destructors, inline code, and optional/template branches. These are real branch arcs from generated code; gcovr has **no option** to exclude “inline” or “destructor” branches.
- **Source files:** Catch blocks, some exception-propagation arcs that aren’t classified as throw-only, and logic branches that need more tests.

**Can branch coverage be increased further without source changes?**

- **Via gcovr only:** No. There are no additional gcovr options that exclude “inline” or “header” branches. You could use `exclude-branches-by-pattern` to hide specific lines by regex, but that’s manual and not scalable.
- **Via tests:** Yes. Add tests for the remaining **logic** branches (e.g. more edge cases, error paths). The **compiler-generated / inline** and **D-Bus catch** branches will stay unless you change design (e.g. injectable bus) or tooling.

### Is there any chance to improve further?

**Yes, but only a little** without changing production code.

| Approach | Effect | Notes |
|----------|--------|--------|
| **More unit tests** | Small gain (e.g. 1–3%) | Target remaining logic branches: more `findWithMask` / `getBootNum` / `incrBootCycle` edge cases, `from_json` combinations, or scenarios that trigger the **cereal::Exception** catch in serialize (e.g. disk full / read-only in more places). Most remaining uncovered branches are not pure logic. |
| **Report-only: exclude-branches-by-pattern** | Can raise *reported* % | In `gcovr.cfg`, add `exclude-branches-by-pattern = <regex>` to hide specific unreachable or known-noise lines. Does not run more code; only changes the report. Manual and brittle. |
| **Design/source changes** | Larger gain | **Injectable D-Bus** (or test double) for `logNvidiaPostCode` so the catch block can be exercised. **Move code out of headers** into .cpp where possible so branch arcs are not duplicated across TUs. Then add tests for the new call paths. |

**What’s left uncovered (and why):**

- **inc/post_code.hpp (~52% branch)**  
  Inlined code, destructors, operators, optional/template branches. gcovr cannot exclude these; covering more would require either moving code to .cpp or accepting that header branches stay partially uncovered.

- **logNvidiaPostCode catch**  
  Only coverable if the D-Bus call can throw in tests (e.g. injectable bus or test double).

- **Exception propagation / compiler arcs**  
  Some branches are exception-handling or compiler-generated; only partly filtered by `exclude-throw-branches` and not practically coverable by more tests alone.

**Bottom line:** You can still eke out a bit more with targeted tests and, if acceptable, with `exclude-branches-by-pattern` for report only. Meaningful further improvement needs design or source changes (injectable bus, less code in headers).

---

## src/post_code.cpp

- **Total branches:** 249  
- **Covered:** 221  
- **Uncovered:** 28  
- **Branch coverage:** 88.8%

| Limitation | Affected functions/area | Approx. % of file's branches not covered | Unblocked by / Reason |
|------------|------------------------|------------------------------------------|------------------------|
| Compiler-generated arcs | Exception handling, dtors, move/copy | ~5–10% | gcov branch arcs in catch blocks and generated code; estimate from partial-branch lines. |
| D-Bus / external | `PostCodeEvent::raise()` try/catch (sdbusplus::exception::throw_via_json, catch generated_event_base) | ~2% | Throw path and catch block exercised in tests; remaining branches in exception propagation. |
| Cereal/filesystem in serialize/deserialize | `serialize`, `deserialize`, `deserializePostCodes` catch blocks | ~5% | Some catch paths covered by existing tests (DeserializeCerealException, SerializeFilesystemError, etc.); remaining branches are duplicate catch or unreachable. |
| Conditional / logic branches | `findWithMask` mask vs exact match, handler secondary optional, getPostCodes/savePostCodes branches | ~3% | Partially covered; remaining are edge cases or compiler branches. |

**Fixable by UT (done):** Added `FromJsonPostCodeEventArgumentSkippedNonStringNonInteger` to cover from_json(PostCodeEvent) branch where argument value is neither string nor integer. Other branches improved by existing tests (decodeHexString invalid, from_json handler/event, serialize/deserialize error paths).

**Real limitation (no prod change):** Exception arcs and some filesystem/cereal error paths that would require forcing stream or OS failures in more scenarios.

---

## inc/post_code.hpp

- **Total branches:** 59  
- **Covered:** 31  
- **Uncovered:** 28  
- **Branch coverage:** 52.5%

| Limitation | Affected functions/area | Approx. % of file's branches not covered | Unblocked by / Reason |
|------------|------------------------|------------------------------------------|------------------------|
| Compiler-generated / inline | Operators, destructors, inline accessors | ~25% | Header inlined in multiple TUs; branch arcs from generated code. *Not* removed by `exclude-throw-branches` / `exclude-unreachable-branches` (those only filter exception arcs and empty-line branches). |
| Template / conditional | Struct and optional usage | ~5% | Test via public API; some branches only in specific instantiations. |

**Fixable by UT (done):** Coverage via tests that use PostCode, PostCodeHandler, PostCodeEvent, and PostCodeHandlers (test_post_code, test_post_code_handlers).

**Real limitation (no prod change):** Header-only branches and compiler-generated code; no production refactor. gcovr does not support excluding “inline” or “destructor” branches.

---

## src/nvidia_post_code_handler.cpp

- **Total branches:** 42  
- **Covered:** 38  
- **Uncovered:** 4  
- **Branch coverage:** 90.5%

| Limitation | Affected functions/area | Approx. % of file's branches not covered | Unblocked by / Reason |
|------------|------------------------|------------------------------------------|------------------------|
| D-Bus / external | `logNvidiaPostCode` try/catch (sdbusplus::bus::new_default, call_noreply; catch std::exception) | ~10% | Catch block: would need injectable bus or way to make D-Bus call throw in UT without production change. |
| Compiler-generated | Exception handling arcs | ~1% | Estimated. |

**Fixable by UT (done):** extractStatusType, extractClass, extractSubclass, extractInstance, extractOpcode, getFirmwareName, getPackageNumber, postcodeToUint32, logNvidiaPostCode happy path and resolution branches covered by test_nvidia_post_code_handler and test_post_code_handlers.

**Real limitation (no prod change):** logNvidiaPostCode’s catch block (silent fail on D-Bus error) requires D-Bus to throw; no injection in production.

---

## src/main.cpp

- **Total branches:** 30  
- **Covered:** 0  
- **Uncovered:** 30  
- **Branch coverage:** 0.0%

| Limitation | Affected functions/area | Approx. % of file's branches not covered | Unblocked by / Reason |
|------------|------------------------|------------------------------------------|------------------------|
| Application entry | main(), option parsing, service startup | 100% | Not in scope for unit tests; integration or system test. |

**Fixable by UT (done):** N/A.

**Real limitation (no prod change):** main.cpp is the application entry point; excluded from UT scope.
