package backend

import "core:os"
import "core:strings"
import "core:testing"
import "core:sync"

backend_test_lock: sync.Mutex

@(test)
identity_persists_across_loads :: proc(t: ^testing.T) {
    sync.lock(&backend_test_lock)
    defer sync.unlock(&backend_test_lock)
    // All test data stays in the build directory, away from real user settings.
    context.allocator = context.temp_allocator
    state = {}
    defer state = {}
    state.state_path = "native/build/identity-test.json"
    defer os.remove(state.state_path)
    legacy := `{"settings":{"alias":"Legacy Catch","popup":true,"last_folder":"Downloads","folders":["Pictures"]},"history":[],"next_id":42}`
    if !testing.expect(t, os.write_entire_file(state.state_path, legacy) == nil) do return
    load()
    if !testing.expect(t, ensure_identity()) do return
    first := strings.clone(state.stored.fingerprint)
    testing.expect_value(t, len(first), 32)
    testing.expect_value(t, state.stored.settings.alias, "Legacy Catch")
    testing.expect_value(t, state.stored.settings.folders[0], "Pictures")
    testing.expect_value(t, state.stored.next_id, 42)

    // Simulate a new process reading the saved settings, before any clean exit.
    state.stored = {}
    load()
    testing.expect_value(t, state.stored.fingerprint, first)
    testing.expect(t, ensure_identity())
    testing.expect_value(t, state.stored.fingerprint, first)
    replace(&state.stored.settings.alias, "Renamed Catch")
    testing.expect(t, persist())
    state.stored = {}
    load()
    testing.expect_value(t, state.stored.settings.alias, "Renamed Catch")
    testing.expect_value(t, state.stored.fingerprint, first)

    // A failed save must not leave an ephemeral identity ready to advertise.
    state.stored.fingerprint = ""
    state.state_path = "native/build/identity-test.json/missing-parent/state.json"
    testing.expect(t, !ensure_identity())
    testing.expect_value(t, state.stored.fingerprint, "")
    state.state_path = "native/build/identity-test.json"
}
