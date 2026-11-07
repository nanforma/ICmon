// secrets.h - camera passwords in the system keyring (libsecret / GNOME Keyring), never in a file.
//
// Built with libsecret when its development files are found (ICMON_HAVE_LIBSECRET); otherwise every call
// reports "keyring not available" and the password lives only in memory for the session.
// The calls block (the keyring may wait for an unlock prompt), so they run in a thread and the result is
// delivered on the GTK main thread.
#pragma once
#include <functional>
#include <string>

namespace icmon {

struct SecretResult {
    bool ok = false;        // the keyring answered (for lookup: and a password was found)
    bool available = true;  // false: no keyring support in this build or no keyring service
    std::string password;   // lookup only
    std::string error;
};

bool keyring_compiled_in();

void secret_lookup(const std::string& camera_id, std::function<void(SecretResult)> done);
void secret_store(const std::string& camera_id, const std::string& label, const std::string& password,
                  std::function<void(SecretResult)> done);
void secret_clear(const std::string& camera_id, std::function<void(SecretResult)> done);

}  // namespace icmon
