#include <string.h>
#include "lizard/builtin.h"
#include "lizard/compiler.h"
#include "lizard/coroutine.h"
#include "test_util.h"

static void check(const char *code, LzStatus expected, const char *output, const char *error) {
    LzVM vm;
    LzBuffer text = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&text), lz_writer_from_buffer(&text));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    LzSource source;
    lz_source_from_string(&source, "coroutine.lz", code);
    LzChunk chunk;
    LzDiagnostics diagnostics;
    lz_chunk_init(&chunk);
    LzStatus status = lz_compile(&vm, &source, &chunk, &diagnostics);
    LZ_CHECK(status == LZ_OK);
    if (status != LZ_OK) {
        fprintf(stderr, "program:\n%s", code);
        for (size_t i = 0; i < diagnostics.count; i++) {
            fprintf(stderr, "%s at %u\n", diagnostics.items[i].message, diagnostics.items[i].offset);
        }
    }
    if (status == LZ_OK) {
        status = lz_vm_run(&vm, &chunk);
        LZ_CHECK(status == expected);
        if (status != expected) fprintf(stderr, "status %d: %s\n", status, vm.error);
        if (output != NULL) LZ_CHECK(strcmp(text.data == NULL ? "" : text.data, output) == 0);
        if (error != NULL) LZ_CHECK(strstr(vm.error, error) != NULL);
        LZ_CHECK(vm.context == &vm.main_context && vm.stack == vm.main_stack);
        LZ_CHECK(vm.frames == vm.main_frames && vm.handlers == vm.main_handlers);
        LZ_CHECK(vm.stack_cap == LZ_STACK_MAX && vm.frame_cap == LZ_FRAMES_MAX);
        LZ_CHECK(vm.native_depth == 0 && vm.stack_count == 0 && vm.frame_count == 0);
        LzString *name = lz_vm_intern(&vm, "co", 2);
        const LzValue *value = name == NULL ? NULL : lz_vm_lookup(&vm, name);
        if (value != NULL && value->type == LZ_VAL_OBJECT && value->as.object->type == LZ_OBJ_COROUTINE) {
            const LzCoroutine *co = (const LzCoroutine *)value->as.object;
            if (co->state == LZ_CO_DONE || co->state == LZ_CO_FAILED) {
                LZ_CHECK(co->context.stack == NULL && co->context.frames == NULL && co->context.handlers == NULL);
                LZ_CHECK(co->entry.type == LZ_VAL_NIL);
            }
        }
    }
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0);
    LZ_CHECK(vm.heap.live_bytes == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&text);
}

static void check_budget(const char *code, uint64_t fuel, size_t heap, LzStatus expected) {
    LzVM vm;
    LzBuffer text = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&text), lz_writer_from_buffer(&text));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    LzSource source;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    lz_source_from_string(&source, "budget.lz", code);
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    lz_vm_set_fuel(&vm, fuel);
    lz_vm_set_heap_limit(&vm, heap);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == expected);
    LZ_CHECK(vm.context == &vm.main_context && vm.native_depth == 0);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_bytes == 0 && vm.heap.live_objects == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&text);
}

static void check_direct_shutdown(void) {
    LzVM vm;
    LzBuffer text = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&text), lz_writer_from_buffer(&text));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    LzSource source;
    lz_source_from_string(&source, "shutdown.lz",
        "let fresh = coroutine(fn() => nil)\n"
        "let co = coroutine(fn() { let local = [1, 2]; let capture = fn() => local; try { yield(capture) } catch e {} })\n"
        "let capture = co.resume()\n");
    LzChunk chunk;
    LzDiagnostics diagnostics;
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    LzString *name = lz_vm_intern(&vm, "fresh", 5);
    const LzValue *fresh = lz_vm_lookup(&vm, name);
    LZ_CHECK(fresh != NULL && ((LzCoroutine *)fresh->as.object)->context.stack == NULL);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_free(&vm);
    LZ_CHECK(vm.heap.live_bytes == 0 && vm.heap.live_objects == 0);
    lz_buffer_free(&text);
}

static void check_open_upvalue_cycle(void) {
    LzVM vm;
    LzBuffer text = {0};
    lz_vm_init(&vm, lz_writer_from_buffer(&text), lz_writer_from_buffer(&text));
    LZ_CHECK(lz_builtin_install(&vm) == LZ_OK);
    const char *code =
        "fn make() { let co = coroutine(fn(self) { let local = self; let capture = fn() => local; yield(capture) }); co.resume(co) }\n"
        "make()\n";
    LzSource source;
    LzChunk chunk;
    LzDiagnostics diagnostics;
    lz_source_from_string(&source, "open-cycle.lz", code);
    lz_chunk_init(&chunk);
    LZ_CHECK(lz_compile(&vm, &source, &chunk, &diagnostics) == LZ_OK);
    LZ_CHECK(lz_vm_run(&vm, &chunk) == LZ_OK);
    lz_skink_sweep(&vm);
    LZ_CHECK(vm.skink.stats.cycles > 0);
    lz_chunk_free(&vm.heap, &chunk);
    lz_vm_release_roots(&vm);
    LZ_CHECK(vm.heap.live_objects == 0 && vm.heap.live_bytes == 0);
    lz_vm_free(&vm);
    lz_buffer_free(&text);
}

int main(void) {
    check("let co: coroutine = coroutine(fn(x = 4, *rest) { println(x, rest); "
          "let y = yield(); println(y); return yield(1, 2) })\n"
          "println(typeof(co), co.to_string(), co.status())\n"
          "println(co.resume(7, 8, 9))\nprintln(co.resume(3, 4))\n"
          "println(co.resume([5, 6]))\nprintln(co.status())\n",
          LZ_OK, "coroutine <coroutine suspended> suspended\n7 [8, 9]\nnil\n[3, 4]\n[1, 2]\n[5, 6]\ndone\n", NULL);
    check("let co = coroutine(fn() { try { yield(1); throw Error(\"kept\") } catch e { return e.message } })\n"
          "println(co.resume())\nprintln(co.resume())\n", LZ_OK, "1\nkept\n", NULL);
    check("let co = coroutine(fn() { let x = 1; yield(fn(v) { x = v }); return x })\n"
          "let change = co.resume()\nchange(42)\nprintln(co.resume())\n", LZ_OK, "42\n", NULL);
    check("let co = coroutine(fn() { fn deep(n) { if n == 0 { return yield(19) }; return deep(n - 1) }; "
          "return deep(80) })\nprintln(co.resume())\nprintln(co.resume(23))\n", LZ_OK, "19\n23\n", NULL);
    check("yield()\n", LZ_ERR_RUNTIME, "", "cannot yield from the main program");
    check("let co = coroutine(fn() => [1].map(fn(x) => yield(x)))\nco.resume()\n",
          LZ_ERR_RUNTIME, "", "cannot yield across a native call");
    check("let co = coroutine(fn() => nil)\nco.resume()\nco.resume()\n",
          LZ_ERR_RUNTIME, "", "cannot resume a coroutine that is done");
    check("let co = coroutine(fn() { throw Error(\"bad\") })\n"
          "try { co.resume() } catch e { println(e.message) }\nprintln(co.status())\nco.resume()\n",
          LZ_ERR_RUNTIME, "bad\nfailed\n", "cannot resume a coroutine that is failed");
    check("let co = coroutine(fn() { throw Error(\"bad\") })\nco.resume()\n",
          LZ_ERR_RUNTIME, "", "bad");
    check("let co = coroutine(fn(x = 3, *rest) => [x, rest])\nprintln(co.resume())\n",
          LZ_OK, "[3, []]\n", NULL);
    check("let box = [nil]\nbox[0] = coroutine(fn() { println(box[0].status()); box[0].resume() })\nbox[0].resume()\n",
          LZ_ERR_RUNTIME, "running\n", "cannot resume a coroutine that is running");
    check("let box = [nil]\nlet child = coroutine(fn() { println(box[0].status()); box[0].resume() })\n"
          "box[0] = coroutine(fn() => child.resume())\nbox[0].resume()\n",
          LZ_ERR_RUNTIME, "normal\n", "cannot resume a coroutine that is normal");
    check("let child = coroutine(fn() { yield(1); return 2 })\n"
          "let parent = coroutine(fn() { yield(child.resume()); return child.resume() })\n"
          "println(parent.resume(), parent.resume())\n", LZ_OK, "1 2\n", NULL);
    check("class V { pub x: int\npub operator <(self, other) => yield(self.x) }\n"
          "let co = coroutine(fn() => sort([V(2), V(1)]))\nco.resume()\n",
          LZ_ERR_RUNTIME, "", "cannot yield across a native call");
    check("fn make() { let co = coroutine(fn() { let box = [nil]; box[0] = fn() => box; yield() }); co.resume() }\nmake()\n",
          LZ_OK, "", NULL);
    check("fn make() { let box = [nil]; let co = coroutine(fn() { try { yield(box) } catch e { return nil } }); "
          "box[0] = co; co.resume() }\nmake()\n", LZ_OK, "", NULL);
    check("let co = coroutine(fn() { try { yield(1) } catch e { return nil } })\nco.resume()\n",
          LZ_OK, "", NULL);
    check("fn recurse(n) { let x = n; let capture = fn() => x; return recurse(n + 1) }\n"
          "let co = coroutine(fn() => recurse(0))\nco.resume()\n", LZ_ERR_STACK_OVERFLOW, "", "stack overflow");
    check("coroutine(print)\n", LZ_ERR_TYPE, "", "expected a Lizard function");
    check("let co = coroutine(fn(a, b = 2, *rest) { yield(a, b, rest); return yield([1, 2]) })\n"
          "println(co.resume(*[4, 5, 6]))\nprintln(co.resume())\nprintln(co.resume())\n", LZ_OK,
          "[4, 5, [6]]\n[1, 2]\nnil\n", NULL);
    check("let co = coroutine(fn(x) => x)\ntry { co.resume() } catch e { println(co.status()) }\nprintln(7)\n",
          LZ_OK, "failed\n7\n", NULL);
    check("fn make() { let co = coroutine(fn() { let x = 17; yield(fn() => x) }); return co.resume() }\n"
          "let get = make()\nprintln(get())\n", LZ_OK, "17\n", NULL);
    check("let coroutine = fn(x) => x + 1\nlet yield = fn(x) => x * 2\nprintln(coroutine(yield(3)))\n",
          LZ_OK, "7\n", NULL);
    check("fn nest() { return coroutine(nest).resume() }\nnest()\n", LZ_ERR_STACK_OVERFLOW, "", "stack overflow");
    check_budget("let co = coroutine(fn() { while true {} })\nco.resume()\n", 100, SIZE_MAX, LZ_ERR_FUEL);
    check_budget("let child = coroutine(fn() { while true {} })\nlet co = coroutine(fn() => child.resume())\nco.resume()\n",
                 100, SIZE_MAX, LZ_ERR_FUEL);
    check_budget("let held = []\nfor i in range(20000) { let co = coroutine(fn() => yield()); co.resume(); held.push(co) }\n",
                 UINT64_MAX, 512 * 1024 * 1024, LZ_ERR_HEAP_LIMIT);
    check_budget("fn make() { let box = [nil]; let co = coroutine(fn() => yield(box)); box[0] = co; co.resume() }\n"
                 "for i in range(2000) { make() }\n", UINT64_MAX, 1024 * 1024, LZ_OK);
    check_direct_shutdown();
    check_open_upvalue_cycle();
    return LZ_TEST_RESULT();
}
