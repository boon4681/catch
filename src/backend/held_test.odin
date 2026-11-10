package backend

import "core:os"
import "core:path/filepath"
import "core:strings"
import "core:testing"
import "core:sync"

@(test)
held_files_require_save_and_discard_cleans_up :: proc(t: ^testing.T) {
    sync.lock(&backend_test_lock)
    defer sync.unlock(&backend_test_lock)
    context.allocator = context.temp_allocator
    state = {}
    defer state = {}
    root :: "native/build/held-test"
    incoming :: root + "/incoming"
    output :: root + "/saved"
    testing.expect(t, os.make_directory_all(incoming) == nil)
    testing.expect(t, os.make_directory_all(output) == nil)
    state.state_path = root + "/state.json"
    defer {
        discard_landed()
        os.remove(incoming + "/a.bin")
        os.remove(incoming + "/b.bin")
        os.remove(output + "/a.bin")
        os.remove(output + "/b.bin")
        os.remove(output + "/a (1).bin")
        os.remove(root + "/state.json")
        os.remove(incoming)
        os.remove(output)
        os.remove(root)
    }
    payload := "test payload"
    testing.expect(t, os.write_entire_file(incoming + "/a.bin", payload) == nil)
    incoming_abs, _ := filepath.abs(incoming, context.temp_allocator)
    file_a, _ := filepath.abs(incoming + "/a.bin", context.temp_allocator)
    file_b, _ := filepath.abs(incoming + "/b.bin", context.temp_allocator)
    output_abs, _ := filepath.abs(output, context.temp_allocator)
    ev := LS_Event{kind = LS_EV_DONE, session = 17, sender = "test", title = "a.bin", mime = "application/octet-stream",
                   dir = cstr(incoming_abs), paths = cstr(file_a), file_count = 1}
    keep_landed(&ev)
    // Receiving and reviewing has not created a destination file or history.
    testing.expect_value(t, history_count(), 0)
    testing.expect_value(t, landed_count(17), 1)
    _, err := os.stat(output + "/a.bin", context.temp_allocator)
    testing.expect(t, err != nil)
    discard(999) // A stale popup must not discard a newer held transfer.
    testing.expect_value(t, landed_count(17), 1)
    discard(17)
    _, err = os.stat(incoming + "/a.bin", context.temp_allocator)
    testing.expect(t, err != nil)
    testing.expect_value(t, history_count(), 0)

    // Save failure retains the unsaved files. Retrying skips files already saved.
    testing.expect(t, os.make_directory_all(incoming) == nil)
    testing.expect(t, os.write_entire_file(incoming + "/a.bin", payload) == nil)
    ev = LS_Event{kind = LS_EV_DONE, session = 18, sender = "test", title = "a.bin", mime = "application/octet-stream",
                  dir = cstr(incoming_abs), paths = cstr(strings.concatenate({file_a, "\n", file_b})), file_count = 2}
    keep_landed(&ev)
    result := save_landed(18, output_abs, "2 files")
    testing.expect_value(t, result.saved, 1)
    testing.expect_value(t, result.failed, 1)
    testing.expect_value(t, landed_count(18), 1)
    testing.expect(t, os.write_entire_file(incoming + "/b.bin", payload) == nil)
    result = save_landed(18, output_abs, "b.bin")
    testing.expect_value(t, result.saved, 1)
    testing.expect_value(t, result.failed, 0)
    testing.expect_value(t, landed_count(18), 0)
    data, read_err := os.read_entire_file(output + "/a.bin", context.temp_allocator)
    testing.expect(t, read_err == nil && string(data) == payload)
    _, err = os.stat(output + "/a (1).bin", context.temp_allocator)
    testing.expect(t, err != nil)
}

@(test)
memory_save_writes_exact_bytes_without_overwrite :: proc(t: ^testing.T) {
    context.allocator = context.temp_allocator
    root :: "native/build/memory-save-test"
    root_abs, _ := filepath.abs(root, context.temp_allocator)
    defer {
        os.remove(root + "/a.bin")
        os.remove(root + "/a (1).bin")
        os.remove(root + "/empty.bin")
        os.remove(root)
    }
    payload := []u8{0, 255, 1, 13, 10, 0, 128}
    buf: [2048]u8
    testing.expect(t, cw_write_into(raw_data(payload), i64(len(payload)), "a.bin", cstr(root_abs), raw_data(buf[:]), len(buf)) != 0)
    first := strings.clone(string(cstring(raw_data(buf[:]))))
    testing.expect(t, cw_write_into(raw_data(payload), i64(len(payload)), "a.bin", cstr(root_abs), raw_data(buf[:]), len(buf)) != 0)
    second := string(cstring(raw_data(buf[:])))
    testing.expect(t, first != second)
    paths := []string{first, second}
    for path in paths {
        data, err := os.read_entire_file(path, context.temp_allocator)
        testing.expect(t, err == nil && string(data) == string(payload))
    }
    testing.expect(t, cw_write_into(nil, 0, "empty.bin", cstr(root_abs), raw_data(buf[:]), len(buf)) != 0)
    info, err := os.stat(root + "/empty.bin", context.temp_allocator)
    testing.expect(t, err == nil && info.size == 0)
}
