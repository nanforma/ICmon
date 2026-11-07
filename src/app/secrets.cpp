#include "secrets.h"

#include <glib.h>

#include <algorithm>
#include <memory>
#include <thread>

#ifdef ICMON_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

namespace icmon {

namespace {

struct Job {
    std::function<void(SecretResult)> done;
    SecretResult r;
};

gboolean deliver(gpointer p) {
    std::unique_ptr<Job> j((Job*)p);
    if (j->done) j->done(std::move(j->r));
    return G_SOURCE_REMOVE;
}

void post(std::function<void(SecretResult)> done, SecretResult r) {
    g_idle_add(deliver, new Job{std::move(done), std::move(r)});
}

}  // namespace

#ifdef ICMON_HAVE_LIBSECRET

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const SecretSchema kSchema = {"org.icmon.Camera",
                                     SECRET_SCHEMA_NONE,
                                     {{"camera_id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"NULL", SECRET_SCHEMA_ATTRIBUTE_STRING}}};

bool keyring_compiled_in() { return true; }

void secret_lookup(const std::string& id, std::function<void(SecretResult)> done) {
    std::thread([id, done = std::move(done)]() mutable {
        SecretResult r;
        GError* err = nullptr;
        gchar* pw = secret_password_lookup_sync(&kSchema, nullptr, &err, "camera_id", id.c_str(), NULL);
        if (err) {
            r.available = false;  // the keyring service could not be used
            r.error = err->message;
            g_error_free(err);
        } else if (pw) {
            r.ok = true;
            r.password = pw;
            secret_password_free(pw);
        }
        post(std::move(done), std::move(r));
    }).detach();
}

void secret_store(const std::string& id, const std::string& label, const std::string& password,
                  std::function<void(SecretResult)> done) {
    std::thread([id, label, pw = password, done = std::move(done)]() mutable {
        SecretResult r;
        GError* err = nullptr;
        gboolean ok = secret_password_store_sync(&kSchema, SECRET_COLLECTION_DEFAULT, label.c_str(), pw.c_str(), nullptr,
                                                 &err, "camera_id", id.c_str(), NULL);
        if (err) {
            r.available = false;
            r.error = err->message;
            g_error_free(err);
        } else {
            r.ok = ok;
        }
        std::fill(pw.begin(), pw.end(), '\0');
        post(std::move(done), std::move(r));
    }).detach();
}

void secret_clear(const std::string& id, std::function<void(SecretResult)> done) {
    std::thread([id, done = std::move(done)]() mutable {
        SecretResult r;
        GError* err = nullptr;
        secret_password_clear_sync(&kSchema, nullptr, &err, "camera_id", id.c_str(), NULL);
        if (err) {
            r.available = false;
            r.error = err->message;
            g_error_free(err);
        } else {
            r.ok = true;
        }
        post(std::move(done), std::move(r));
    }).detach();
}

#else  // no libsecret in this build

bool keyring_compiled_in() { return false; }

static void unavailable(std::function<void(SecretResult)> done) {
    SecretResult r;
    r.available = false;
    r.error = "this build has no keyring support (install libsecret-1-dev and rebuild)";
    post(std::move(done), std::move(r));
}

void secret_lookup(const std::string&, std::function<void(SecretResult)> done) { unavailable(std::move(done)); }
void secret_store(const std::string&, const std::string&, const std::string&, std::function<void(SecretResult)> done) {
    unavailable(std::move(done));
}
void secret_clear(const std::string&, std::function<void(SecretResult)> done) { unavailable(std::move(done)); }

#endif

}  // namespace icmon
