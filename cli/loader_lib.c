// This file is a part of Julia. License is MIT: https://julialang.org/license

// This file defines an RPATH-style relative path loader for all platforms
#include "loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring in definitions of symbols exported from libjulia. */
#include "jl_exports.h"

/* Bring in helper functions for windows without libgcc. */
#ifdef _OS_WINDOWS_
#include "loader_win_utils.c"

#include <fileapi.h>
static int win_file_exists(wchar_t* wpath) {
  return GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES ? 0 : 1;
}
#endif

// Save DEP_LIBS to a variable that is explicitly sized for expansion
static char dep_libs[1024] = "\0" DEP_LIBS;

JL_DLLEXPORT void jl_loader_print_stderr(const char * msg)
{
    fputs(msg, stderr);
}
// I use three arguments a lot.
void jl_loader_print_stderr3(const char * msg1, const char * msg2, const char * msg3)
{
    jl_loader_print_stderr(msg1);
    jl_loader_print_stderr(msg2);
    jl_loader_print_stderr(msg3);
}

/* Wrapper around dlopen(), with extra relative pathing thrown in*/
/* If err, then loads the library successfully or panics.
 * If !err, then loads the library or returns null if the file does not exist,
 * or panics if opening failed for any other reason. */
/* Currently the only use of this function with !err is in opening libjulia-codegen,
 * which the user can delete to save space if generating new code is not necessary.
 * However, if it exists and cannot be loaded, that's a problem. So, we alert the user
 * and abort the process. */
static void * load_library(const char * rel_path, const char * src_dir, int err) {
    void * handle = NULL;
    // See if a handle is already open to the basename
    const char *basename = rel_path + strlen(rel_path);
    while (basename-- > rel_path)
        if (*basename == PATHSEPSTRING[0] || *basename == '/')
            break;
    basename++;
#if defined(_OS_WINDOWS_)
    if ((handle = GetModuleHandleA(basename)))
        return handle;
#else
    // if err == 0 the library is optional, so don't allow global lookups to see it
    if ((handle = dlopen(basename, RTLD_NOLOAD | RTLD_NOW | (err ? RTLD_GLOBAL : RTLD_LOCAL))))
        return handle;
#endif

    char path[2*JL_PATH_MAX + 1] = {0};
    strncat(path, src_dir, sizeof(path) - 1);
    strncat(path, PATHSEPSTRING, sizeof(path) - 1);
    strncat(path, rel_path, sizeof(path) - 1);

#if defined(_OS_WINDOWS_)
#define PATH_EXISTS() win_file_exists(wpath)
    wchar_t wpath[2*JL_PATH_MAX + 1] = {0};
    if (!utf8_to_wchar(path, wpath, 2*JL_PATH_MAX)) {
        jl_loader_print_stderr3("ERROR: Unable to convert path ", path, " to wide string!\n");
        exit(1);
    }
    handle = (void *)LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
#define PATH_EXISTS() !access(path, F_OK)
    handle = dlopen(path, RTLD_NOW | (err ? RTLD_GLOBAL : RTLD_LOCAL));
#endif
    if (handle == NULL) {
        if (!err && !PATH_EXISTS())
            return NULL;
#undef PATH_EXISTS
        jl_loader_print_stderr3("ERROR: Unable to load dependent library ", path, "\n");
#if defined(_OS_WINDOWS_)
        LPWSTR wmsg = TEXT("");
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS |
                       FORMAT_MESSAGE_MAX_WIDTH_MASK,
                       NULL, GetLastError(),
                       MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                       (LPWSTR)&wmsg, 0, NULL);
        char err[256] = {0};
        wchar_to_utf8(wmsg, err, 255);
        jl_loader_print_stderr3("Message:", err, "\n");
#else
        char *dlerr = dlerror();
        if (dlerr != NULL) {
            jl_loader_print_stderr3("Message:", dlerr, "\n");
        }
#endif
        exit(1);
    }
    return handle;
}

static void * lookup_symbol(const void * lib_handle, const char * symbol_name) {
#ifdef _OS_WINDOWS_
    return GetProcAddress((HMODULE) lib_handle, symbol_name);
#else
    return dlsym((void *)lib_handle, symbol_name);
#endif
}

// Find the location of libjulia.
char lib_dir[JL_PATH_MAX];
JL_DLLEXPORT const char * jl_get_libdir()
{
    // Reuse the path if this is not the first call.
    if (lib_dir[0] != 0) {
        return lib_dir;
    }
#if defined(_OS_WINDOWS_)
    // On Windows, we use GetModuleFileNameW
    wchar_t libjulia_path[JL_PATH_MAX];
    HMODULE libjulia = NULL;

    // Get a handle to libjulia.
    if (!utf8_to_wchar(LIBJULIA_NAME, libjulia_path, JL_PATH_MAX)) {
        jl_loader_print_stderr3("ERROR: Unable to convert path ", LIBJULIA_NAME, " to wide string!\n");
        exit(1);
    }
    libjulia = LoadLibraryW(libjulia_path);
    if (libjulia == NULL) {
        jl_loader_print_stderr3("ERROR: Unable to load ", LIBJULIA_NAME, "!\n");
        exit(1);
    }
    if (!GetModuleFileNameW(libjulia, libjulia_path, JL_PATH_MAX)) {
        jl_loader_print_stderr("ERROR: GetModuleFileName() failed\n");
        exit(1);
    }
    if (!wchar_to_utf8(libjulia_path, lib_dir, JL_PATH_MAX)) {
        jl_loader_print_stderr("ERROR: Unable to convert julia path to UTF-8\n");
        exit(1);
    }
#else
    // On all other platforms, use dladdr()
    Dl_info info;
    if (!dladdr(&jl_get_libdir, &info)) {
        jl_loader_print_stderr("ERROR: Unable to dladdr(&jl_get_libdir)!\n");
        char *dlerr = dlerror();
        if (dlerr != NULL) {
            jl_loader_print_stderr3("Message:", dlerr, "\n");
        }
        exit(1);
    }
    strcpy(lib_dir, info.dli_fname);
#endif
    // Finally, convert to dirname
    const char * new_dir = dirname(lib_dir);
    if (new_dir != lib_dir) {
        // On some platforms, dirname() mutates.  On others, it does not.
        memcpy(lib_dir, new_dir, strlen(new_dir)+1);
    }
    return lib_dir;
}

// On Linux, it can happen that the system has a newer libstdc++ than the one we ship,
// which can break loading of some system libraries: <https://github.com/JuliaLang/julia/issues/34276>.
// As a fix, on linux we probe the system libstdc++ to see if it is newer, and then load it if it is.
// Otherwise, we load the bundled one. This improves compatibility with third party dynamic libs that
// may depend on symbols exported by the system libstdxc++.
#ifdef _OS_LINUX_
#ifndef GLIBCXX_LEAST_VERSION_SYMBOL
#warning GLIBCXX_LEAST_VERSION_SYMBOL should always be defined in the makefile.
#define GLIBCXX_LEAST_VERSION_SYMBOL "GLIBCXX_a.b.c" /* Appease the linter */
#endif

#include <link.h>
#include <sys/wait.h>

// write(), but handle errors and avoid EINTR
static void write_wrapper(int fd, const char *str, size_t len)
{
    size_t written_sofar = 0;
    while (len) {
        ssize_t bytes_written = write(fd, str + written_sofar, len);
        if (bytes_written == -1 && errno == EINTR) continue;
        if (bytes_written == -1 && errno != EINTR) {
            perror("(julia) child libstdcxxprobe write");
            _exit(1);
        }
        len -= bytes_written;
        written_sofar += bytes_written;
    }
}

// read(), but handle errors and avoid EINTR
static void read_wrapper(int fd, char **ret, size_t *ret_len)
{
    // Allocate an initial buffer
    size_t len = JL_PATH_MAX;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        perror("(julia) malloc");
        exit(1);
    }

    // Read into it, reallocating as necessary
    size_t have_read = 0;
    while (1) {
        ssize_t n = read(fd, buf + have_read, len - have_read);
        have_read += n;
        if (n == 0) break;
        if (n == -1 && errno != EINTR) {
            perror("(julia) libstdcxxprobe read");
            exit(1);
        }
        if (n == -1 && errno == EINTR) continue;
        if (have_read == len) {
            buf = (char *)realloc(buf, 1 + (len *= 2));
            if (!buf) {
                perror("(julia) realloc");
                exit(1);
            }
        }
    }

    *ret = buf;
    *ret_len = have_read;
}

// Return the path to the libstdcxx to load.
// If the path is found, return it.
// Otherwise, print the error and exit.
// The path returned must be freed.
static char *libstdcxxprobe(void)
{
    // Create the pipe and child process.
    int fork_pipe[2];
    int ret = pipe(fork_pipe);
    if (ret == -1) {
        perror("(julia) Error during libstdcxxprobe: pipe");
        exit(1);
    }
    pid_t pid = fork();
    if (pid == -1)  {
        perror("Error during libstdcxxprobe:\nfork");
        exit(1);
    }
    if (pid == (pid_t) 0) { // Child process.
        close(fork_pipe[0]);

        // Open the first available libstdc++.so.
        // If it can't be found, report so by exiting zero.
        // The star is there to prevent the compiler from merging constants
        // with "\0*libstdc++.so.6", which we string replace inside the .so during
        // make install.
        void *handle = dlopen("libstdc++.so.6\0*", RTLD_LAZY);
        if (!handle) {
            _exit(0);
        }

        // See if the version is compatible
        char *dlerr = dlerror(); // clear out dlerror
        void *sym = dlsym(handle, GLIBCXX_LEAST_VERSION_SYMBOL);
        dlerr = dlerror();
        if (dlerr) {
            // We can't use the library that was found, so don't write anything.
            // The main process will see that nothing was written,
            // then exit the function and return null.
            _exit(0);
        }

        // No error means the symbol was found, we can use this library.
        // Get the path to it, and write it to the parent process.
        struct link_map *lm;
        ret = dlinfo(handle, RTLD_DI_LINKMAP, &lm);
        if (ret == -1) {
            char *errbuf = dlerror();
            char *errdesc = (char*)"Error during libstdcxxprobe in child process:\ndlinfo: ";
            write_wrapper(STDERR_FILENO, errdesc, strlen(errdesc));
            write_wrapper(STDERR_FILENO, errbuf, strlen(errbuf));
            write_wrapper(STDERR_FILENO, "\n", 1);
            _exit(1);
        }
        char *libpath = lm->l_name;
        write_wrapper(fork_pipe[1], libpath, strlen(libpath));
        _exit(0);
    }
    else { // Parent process.
        close(fork_pipe[1]);

        // Read the absolute path to the lib from the child process.
        char *path;
        size_t pathlen;
        read_wrapper(fork_pipe[0], &path, &pathlen);

        // Close the read end of the pipe
        close(fork_pipe[0]);

        // Wait for the child to complete.
        while (1) {
            int wstatus;
            pid_t npid = waitpid(pid, &wstatus, 0);
            if (npid == -1) {
                if (errno == EINTR) continue;
                if (errno != EINTR) {
                    perror("Error during libstdcxxprobe in parent process:\nwaitpid");
                    exit(1);
                }
            }
            else if (!WIFEXITED(wstatus)) {
                const char *err_str = "Error during libstdcxxprobe in parent process:\n"
                                      "The child process did not exit normally.\n";
                size_t err_strlen = strlen(err_str);
                write_wrapper(STDERR_FILENO, err_str, err_strlen);
                exit(1);
            }
            else if (WEXITSTATUS(wstatus)) {
                // The child has printed an error and exited, so the parent should exit too.
                exit(1);
            }
            break;
        }

        if (!pathlen) {
            free(path);
            return NULL;
        }
        return path;
    }
}
#endif

void * libjulia_internal = NULL;
__attribute__((constructor)) void jl_load_libjulia_internal(void) {
    // Only initialize this once
    if (libjulia_internal != NULL) {
        return;
    }

    // Introspect to find our own path
    const char *lib_dir = jl_get_libdir();

    // Pre-load libraries that libjulia-internal needs.
    int deps_len = strlen(&dep_libs[1]);
    char *curr_dep = &dep_libs[1];

    void *cxx_handle;

#if defined(_OS_LINUX_)
    int do_probe = 1;
    int done_probe = 0;
    char *probevar = getenv("JULIA_PROBE_LIBSTDCXX");
    if (probevar) {
        if (strcmp(probevar, "1") == 0 || strcmp(probevar, "yes") == 0)
            do_probe = 1;
        else if (strcmp(probevar, "0") == 0 || strcmp(probevar, "no") == 0)
            do_probe = 0;
    }
    if (do_probe) {
        char *cxxpath = libstdcxxprobe();
        if (cxxpath) {
            cxx_handle = dlopen(cxxpath, RTLD_LAZY);
            char *dlr = dlerror();
            if (dlr) {
                jl_loader_print_stderr("ERROR: Unable to dlopen(cxxpath) in parent!\n");
                jl_loader_print_stderr3("Message: ", dlr, "\n");
                exit(1);
            }
            free(cxxpath);
            done_probe = 1;
        }
    }
    if (!done_probe) {
        const static char bundled_path[256] = "\0*libstdc++.so.6";
        load_library(&bundled_path[2], lib_dir, 1);
    }
#endif

    // We keep track of "special" libraries names (ones whose name is prefixed with `@`)
    // which are libraries that we want to load in some special, custom way, such as
    // `libjulia-internal` or `libjulia-codegen`.
    int special_idx = 0;
    char * special_library_names[2] = {NULL};
    while (1) {
        // try to find next colon character; if we can't, break out
        char * colon = strchr(curr_dep, ':');
        if (colon == NULL)
            break;

        // Chop the string at the colon so it's a valid-ending-string
        *colon = '\0';

        // If this library name starts with `@`, don't open it here (but mark it as special)
        if (curr_dep[0] == '@') {
            if (special_idx > sizeof(special_library_names)/sizeof(char *)) {
                jl_loader_print_stderr("ERROR: Too many special library names specified, check LOADER_BUILD_DEP_LIBS and friends!\n");
                exit(1);
            }
            special_library_names[special_idx] = curr_dep + 1;
            special_idx += 1;
        }
        else {
            load_library(curr_dep, lib_dir, 1);
        }

        // Skip ahead to next dependency
        curr_dep = colon + 1;
    }

    if (special_idx != sizeof(special_library_names)/sizeof(char *)) {
        jl_loader_print_stderr("ERROR: Too few special library names specified, check LOADER_BUILD_DEP_LIBS and friends!\n");
        exit(1);
    }

    // Unpack our special library names.  This is why ordering of library names matters.
    libjulia_internal = load_library(special_library_names[0], lib_dir, 1);
    void *libjulia_codegen = load_library(special_library_names[1], lib_dir, 0);
    const char * const * codegen_func_names;
    const char *codegen_liberr;
    if (libjulia_codegen == NULL) {
        // if codegen is not available, use fallback implementation in libjulia-internal
        libjulia_codegen = libjulia_internal;
        codegen_func_names = jl_codegen_fallback_func_names;
        codegen_liberr = " from libjulia-internal\n";
    }
    else {
        codegen_func_names = jl_codegen_exported_func_names;
        codegen_liberr = " from libjulia-codegen\n";
    }

    // Once we have libjulia-internal loaded, re-export its symbols:
    for (unsigned int symbol_idx=0; jl_runtime_exported_func_names[symbol_idx] != NULL; ++symbol_idx) {
        void *addr = lookup_symbol(libjulia_internal, jl_runtime_exported_func_names[symbol_idx]);
        if (addr == NULL) {
            jl_loader_print_stderr3("ERROR: Unable to load ", jl_runtime_exported_func_names[symbol_idx], " from libjulia-internal\n");
            exit(1);
        }
        (*jl_runtime_exported_func_addrs[symbol_idx]) = addr;
    }
    // jl_options must be initialized very early, in case an embedder sets some
    // values there before calling jl_init
    ((void (*)(void))jl_init_options_addr)();

    for (unsigned int symbol_idx=0; codegen_func_names[symbol_idx] != NULL; ++symbol_idx) {
        void *addr = lookup_symbol(libjulia_codegen, codegen_func_names[symbol_idx]);
        if (addr == NULL) {
            jl_loader_print_stderr3("ERROR: Unable to load ", codegen_func_names[symbol_idx], codegen_liberr);
            exit(1);
        }
        (*jl_codegen_exported_func_addrs[symbol_idx]) = addr;
    }
    // Next, if we're on Linux/FreeBSD, set up fast TLS.
#if !defined(_OS_WINDOWS_) && !defined(_OS_DARWIN_)
    void (*jl_pgcstack_setkey)(void*, void*(*)(void)) = lookup_symbol(libjulia_internal, "jl_pgcstack_setkey");
    if (jl_pgcstack_setkey == NULL) {
        jl_loader_print_stderr("ERROR: Cannot find jl_pgcstack_setkey() function within libjulia-internal!\n");
        exit(1);
    }
    void *fptr = lookup_symbol(RTLD_DEFAULT, "jl_get_pgcstack_static");
    void *(*key)(void) = lookup_symbol(RTLD_DEFAULT, "jl_pgcstack_addr_static");
    if (fptr != NULL && key != NULL)
        jl_pgcstack_setkey(fptr, key);
#endif

    // jl_options must be initialized very early, in case an embedder sets some
    // values there before calling jl_init
    ((void (*)(void))jl_init_options_addr)();
}

// Load libjulia and run the REPL with the given arguments (in UTF-8 format)
JL_DLLEXPORT int jl_load_repl(int argc, char * argv[]) {
    // Some compilers/platforms are known to have `__attribute__((constructor))` issues,
    // so we have a fallback call of `jl_load_libjulia_internal()` here.
    if (libjulia_internal == NULL) {
        jl_load_libjulia_internal();
        if (libjulia_internal == NULL) {
            jl_loader_print_stderr("ERROR: libjulia-internal could not be loaded!\n");
            exit(1);
        }
    }
    // Load the repl entrypoint symbol and jump into it!
    int (*entrypoint)(int, char **) = (int (*)(int, char **))lookup_symbol(libjulia_internal, "jl_repl_entrypoint");
    if (entrypoint == NULL) {
        jl_loader_print_stderr("ERROR: Unable to find `jl_repl_entrypoint()` within libjulia-internal!\n");
        exit(1);
    }
    return entrypoint(argc, (char **)argv);
}

#ifdef _OS_WINDOWS_
int __stdcall DllMainCRTStartup(void *instance, unsigned reason, void *reserved) {
    setup_stdio();

    // Because we override DllMainCRTStartup, we have to manually call our constructor methods
    jl_load_libjulia_internal();
    return 1;
}
#endif

#ifdef __cplusplus
} // extern "C"
#endif
