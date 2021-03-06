#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sysexits.h>

#if defined(READLINE)
#include <readline/readline.h>
#elif defined(EDITLINE)
#include <histedit.h>
#endif

#include "mpw-algorithm.h"
#include "mpw-util.h"
#include "mpw-marshall.h"

#ifndef MP_VERSION
#define MP_VERSION ?
#endif
#define MP_ENV_fullName     "MP_FULLNAME"
#define MP_ENV_algorithm    "MP_ALGORITHM"
#define MP_ENV_format       "MP_FORMAT"

static void usage() {

    inf( ""
            "  Master Password v%s\n"
            "--------------------------------------------------------------------------------\n"
            "      https://masterpasswordapp.com\n\n", stringify_def( MP_VERSION ) );
    inf( ""
            "USAGE\n"
            "  mpw [-u|-U full-name] [-m fd] [-t pw-type] [-P value] [-c counter]\n"
            "      [-a version] [-p purpose] [-C context] [-f|-F format] [-R 0|1]\n"
            "      [-v|-q] [-h] site-name\n\n" );
    inf( ""
            "  -u full-name Specify the full name of the user.\n"
            "               -u checks the master password against the config,\n"
            "               -U allows updating to a new master password.\n"
            "               Defaults to %s in env or prompts.\n\n", MP_ENV_fullName );
    dbg( ""
            "  -M master-pw Specify the master password of the user.\n"
            "               Passing secrets as arguments is unsafe, for use in testing only.\n" );
    inf( ""
            "  -m fd        Read the master password of the user from a file descriptor.\n"
            "               Tip: don't send extra characters like newlines such as by using\n"
            "               echo in a pipe.  Consider printf instead.\n\n" );
    inf( ""
            "  -t pw-type   Specify the password's template.\n"
            "               Defaults to 'long' (-p a), 'name' (-p i) or 'phrase' (-p r).\n"
            "                   x, maximum  | 20 characters, contains symbols.\n"
            "                   l, long     | Copy-friendly, 14 characters, symbols.\n"
            "                   m, medium   | Copy-friendly, 8 characters, symbols.\n"
            "                   b, basic    | 8 characters, no symbols.\n"
            "                   s, short    | Copy-friendly, 4 characters, no symbols.\n"
            "                   i, pin      | 4 numbers.\n"
            "                   n, name     | 9 letter name.\n"
            "                   p, phrase   | 20 character sentence.\n"
            "                   K, key      | encryption key (set key size -s bits).\n"
            "                   P, personal | saved personal password (save with -s pw).\n\n" );
    inf( ""
            "  -P value     The parameter value.\n"
            "                   -p i        | The login name for the site.\n"
            "                   -t K        | The bit size of the key to generate (eg. 256).\n"
            "                   -t P        | The personal password to encrypt.\n\n" );
    inf( ""
            "  -c counter   The value of the counter.\n"
            "               Defaults to 1.\n\n" );
    inf( ""
            "  -a version   The algorithm version to use, %d - %d.\n"
            "               Defaults to %s in env or %d.\n\n",
            MPAlgorithmVersionFirst, MPAlgorithmVersionLast, MP_ENV_algorithm, MPAlgorithmVersionCurrent );
    inf( ""
            "  -p purpose   The purpose of the generated token.\n"
            "               Defaults to 'auth'.\n"
            "                   a, auth     | An authentication token such as a password.\n"
            "                   i, ident    | An identification token such as a username.\n"
            "                   r, rec      | A recovery token such as a security answer.\n\n" );
    inf( ""
            "  -C context   A purpose-specific context.\n"
            "               Defaults to empty.\n"
            "                   -p a        | -\n"
            "                   -p i        | -\n"
            "                   -p r        | Most significant word in security question.\n\n" );
    inf( ""
            "  -f|F format  The mpsites format to use for reading/writing site parameters.\n"
            "               -F forces the use of the given format,\n"
            "               -f allows fallback/migration.\n"
            "               Defaults to %s in env or json, falls back to plain.\n"
            "                   n, none     | No file\n"
            "                   f, flat     | ~/.mpw.d/Full Name.%s\n"
            "                   j, json     | ~/.mpw.d/Full Name.%s\n\n",
            MP_ENV_format, mpw_marshall_format_extension( MPMarshallFormatFlat ), mpw_marshall_format_extension( MPMarshallFormatJSON ) );
    inf( ""
            "  -R redacted  Whether to save the mpsites in redacted format or not.\n"
            "               Defaults to 1, redacted.\n\n" );
    inf( ""
            "  -v           Increase output verbosity (can be repeated).\n"
            "  -q           Decrease output verbosity (can be repeated).\n\n" );
    inf( ""
            "ENVIRONMENT\n\n"
            "  %-12s The full name of the user (see -u).\n"
            "  %-12s The default algorithm version (see -a).\n"
            "  %-12s The default mpsites format (see -f).\n\n",
            MP_ENV_fullName, MP_ENV_algorithm, MP_ENV_format );
    exit( 0 );
}

static const char *mpw_getenv(const char *variableName) {

    char *envBuf = getenv( variableName );
    return envBuf? strdup( envBuf ): NULL;
}

static const char *mpw_getline(const char *prompt) {

    fprintf( stderr, "%s ", prompt );

    char *buf = NULL;
    size_t bufSize = 0;
    ssize_t lineSize = getline( &buf, &bufSize, stdin );
    if (lineSize <= 1) {
        free( buf );
        return NULL;
    }

    // Remove the newline.
    buf[lineSize - 1] = '\0';
    return buf;
}

static const char *mpw_getpass(const char *prompt) {

    char *passBuf = getpass( prompt );
    if (!passBuf)
        return NULL;

    char *buf = strdup( passBuf );
    bzero( passBuf, strlen( passBuf ) );
    return buf;
}

static const char *mpw_path(const char *prefix, const char *extension) {

    // Resolve user's home directory.
    char *homeDir = NULL;
    if (!homeDir)
        if ((homeDir = getenv( "HOME" )))
            homeDir = strdup( homeDir );
    if (!homeDir)
        if ((homeDir = getenv( "USERPROFILE" )))
            homeDir = strdup( homeDir );
    if (!homeDir) {
        const char *homeDrive = getenv( "HOMEDRIVE" ), *homePath = getenv( "HOMEPATH" );
        if (homeDrive && homePath)
            homeDir = strdup( mpw_str( "%s%s", homeDrive, homePath ) );
    }
    if (!homeDir) {
        struct passwd *passwd = getpwuid( getuid() );
        if (passwd)
            homeDir = strdup( passwd->pw_dir );
    }
    if (!homeDir)
        homeDir = getcwd( NULL, 0 );

    // Compose filename.
    char *path = strdup( mpw_str( "%s.%s", prefix, extension ) );

    // This is a filename, remove all potential directory separators.
    for (char *slash; (slash = strstr( path, "/" )); *slash = '_');

    // Compose pathname.
    if (homeDir) {
        const char *homePath = mpw_str( "%s/.mpw.d/%s", homeDir, path );
        free( homeDir );
        free( path );

        if (homePath)
            path = strdup( homePath );
    }

    return path;
}

static bool mpw_mkdirs(const char *filePath) {

    if (!filePath)
        return false;

    char *pathEnd = strrchr( filePath, '/' );
    char *path = pathEnd? strndup( filePath, pathEnd - filePath ): strdup( filePath );
    if (!path)
        return false;

    // Save the cwd and for absolute paths, start at the root.
    char *cwd = getcwd( NULL, 0 );
    if (*filePath == '/')
        chdir( "/" );

    // Walk the path.
    bool success = true;
    for (char *dirName = strtok( path, "/" ); success && dirName; dirName = strtok( NULL, "/" )) {
        if (!strlen( dirName ))
            continue;

        success &= (mkdir( dirName, 0700 ) != ERR || errno == EEXIST) && chdir( dirName ) != ERR;
    }
    free( path );

    if (chdir( cwd ) == ERR)
        wrn( "Could not restore cwd:\n  %s: %s\n", cwd, strerror( errno ) );
    free( cwd );

    return success;
}

static char *mpw_read_file(FILE *file) {

    char *buf = NULL;
    size_t blockSize = 4096, bufSize = 0, bufOffset = 0, readSize = 0;
    while ((mpw_realloc( &buf, &bufSize, blockSize )) &&
           (bufOffset += (readSize = fread( buf + bufOffset, 1, blockSize, file ))) &&
           (readSize == blockSize));

    return buf;
}

int main(const int argc, char *const argv[]) {

    // CLI defaults.
    bool allowPasswordUpdate = false, sitesFormatFixed = false;

    // Read the environment.
    const char *fullNameArg = NULL, *masterPasswordFDArg = NULL, *masterPasswordArg = NULL, *siteNameArg = NULL;
    const char *resultTypeArg = NULL, *resultParamArg = NULL, *siteCounterArg = NULL, *algorithmVersionArg = NULL;
    const char *keyPurposeArg = NULL, *keyContextArg = NULL, *sitesFormatArg = NULL, *sitesRedactedArg = NULL;
    fullNameArg = mpw_getenv( MP_ENV_fullName );
    algorithmVersionArg = mpw_getenv( MP_ENV_algorithm );
    sitesFormatArg = mpw_getenv( MP_ENV_format );

    // Read the command-line options.
    for (int opt; (opt = getopt( argc, argv, "u:U:m:M:t:P:c:a:p:C:f:F:R:vqh" )) != EOF;)
        switch (opt) {
            case 'u':
                fullNameArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                allowPasswordUpdate = false;
                break;
            case 'U':
                fullNameArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                allowPasswordUpdate = true;
                break;
            case 'm':
                masterPasswordFDArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'M':
                // Passing your master password via the command-line is insecure.  Testing purposes only.
                masterPasswordArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 't':
                resultTypeArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'P':
                resultParamArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'c':
                siteCounterArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'a':
                algorithmVersionArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'p':
                keyPurposeArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'C':
                keyContextArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'f':
                sitesFormatArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                sitesFormatFixed = false;
                break;
            case 'F':
                sitesFormatArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                sitesFormatFixed = true;
                break;
            case 'R':
                sitesRedactedArg = optarg && strlen( optarg )? strdup( optarg ): NULL;
                break;
            case 'v':
                ++mpw_verbosity;
                break;
            case 'q':
                --mpw_verbosity;
                break;
            case 'h':
                usage();
                break;
            case '?':
                switch (optopt) {
                    case 'u':
                        ftl( "Missing full name to option: -%c\n", optopt );
                        return EX_USAGE;
                    case 't':
                        ftl( "Missing type name to option: -%c\n", optopt );
                        return EX_USAGE;
                    case 'c':
                        ftl( "Missing counter value to option: -%c\n", optopt );
                        return EX_USAGE;
                    default:
                        ftl( "Unknown option: -%c\n", optopt );
                        return EX_USAGE;
                }
            default:
                ftl( "Unexpected option: %c\n", opt );
                return EX_USAGE;
        }
    if (optind < argc && argv[optind])
        siteNameArg = strdup( argv[optind] );

    // Determine fullName, siteName & masterPassword.
    const char *fullName = NULL, *masterPassword = NULL, *siteName = NULL;
    if ((!fullName || !strlen( fullName )) && fullNameArg)
        fullName = strdup( fullNameArg );
    while (!fullName || !strlen( fullName ))
        fullName = mpw_getline( "Your full name:" );
    if ((!masterPassword || !strlen( masterPassword )) && masterPasswordFDArg) {
        FILE *masterPasswordFile = fdopen( atoi( masterPasswordFDArg ), "r" );
        if (!masterPasswordFile)
            wrn( "Error opening master password FD %s: %s\n", masterPasswordFDArg, strerror( errno ) );
        else {
            masterPassword = mpw_read_file( masterPasswordFile );
            if (ferror( masterPasswordFile ))
                wrn( "Error reading master password from %s: %d\n", masterPasswordFDArg, ferror( masterPasswordFile ) );
        }
    }
    if ((!masterPassword || !strlen( masterPassword )) && masterPasswordArg)
        masterPassword = strdup( masterPasswordArg );
    while (!masterPassword || !strlen( masterPassword ))
        masterPassword = mpw_getpass( "Your master password: " );
    if ((!siteName || !strlen( siteName )) && siteNameArg)
        siteName = strdup( siteNameArg );
    while (!siteName || !strlen( siteName ))
        siteName = mpw_getline( "Site name:" );
    MPMarshallFormat sitesFormat = MPMarshallFormatDefault;
    if (sitesFormatArg) {
        sitesFormat = mpw_formatWithName( sitesFormatArg );
        if (ERR == (int)sitesFormat) {
            ftl( "Invalid sites format: %s\n", sitesFormatArg );
            mpw_free_strings( &fullName, &masterPassword, &siteName, NULL );
            return EX_USAGE;
        }
    }
    MPKeyPurpose keyPurpose = MPKeyPurposeAuthentication;
    if (keyPurposeArg) {
        keyPurpose = mpw_purposeWithName( keyPurposeArg );
        if (ERR == (int)keyPurpose) {
            ftl( "Invalid purpose: %s\n", keyPurposeArg );
            return EX_USAGE;
        }
    }
    const char *keyContext = NULL;
    if (keyContextArg)
        keyContext = strdup( keyContextArg );

    // Find the user's sites file.
    FILE *sitesFile = NULL;
    const char *sitesPath = mpw_path( fullName, mpw_marshall_format_extension( sitesFormat ) );
    if (!sitesPath || !(sitesFile = fopen( sitesPath, "r" ))) {
        dbg( "Couldn't open configuration file:\n  %s: %s\n", sitesPath, strerror( errno ) );

        // Try to fall back to the flat format.
        if (!sitesFormatFixed) {
            mpw_free_string( &sitesPath );
            sitesPath = mpw_path( fullName, mpw_marshall_format_extension( MPMarshallFormatFlat ) );
            if (sitesPath && (sitesFile = fopen( sitesPath, "r" )))
                sitesFormat = MPMarshallFormatFlat;
            else
                dbg( "Couldn't open configuration file:\n  %s: %s\n", sitesPath, strerror( errno ) );
        }
    }

    // Load the user object from file.
    MPMarshalledUser *user = NULL;
    MPMarshalledSite *site = NULL;
    MPMarshalledQuestion *question = NULL;
    if (!sitesFile)
        mpw_free_string( &sitesPath );

    else {
        // Read file.
        char *sitesInputData = mpw_read_file( sitesFile );
        if (ferror( sitesFile ))
            wrn( "Error while reading configuration file:\n  %s: %d\n", sitesPath, ferror( sitesFile ) );
        fclose( sitesFile );

        // Parse file.
        MPMarshallInfo *sitesInputInfo = mpw_marshall_read_info( sitesInputData );
        MPMarshallFormat sitesInputFormat = sitesFormatArg? sitesFormat: sitesInputInfo->format;
        MPMarshallError marshallError = { .type = MPMarshallSuccess };
        mpw_marshal_info_free( &sitesInputInfo );
        user = mpw_marshall_read( sitesInputData, sitesInputFormat, masterPassword, &marshallError );
        if (marshallError.type == MPMarshallErrorMasterPassword) {
            // Incorrect master password.
            if (!allowPasswordUpdate) {
                ftl( "Incorrect master password according to configuration:\n  %s: %s\n", sitesPath, marshallError.description );
                mpw_marshal_free( &user );
                mpw_free_strings( &sitesInputData, &sitesPath, &fullName, &masterPassword, &siteName, NULL );
                return EX_DATAERR;
            }

            // Update user's master password.
            while (marshallError.type == MPMarshallErrorMasterPassword) {
                inf( "Given master password does not match configuration.\n" );
                inf( "To update the configuration with this new master password, first confirm the old master password.\n" );

                const char *importMasterPassword = NULL;
                while (!importMasterPassword || !strlen( importMasterPassword ))
                    importMasterPassword = mpw_getpass( "Old master password: " );

                mpw_marshal_free( &user );
                user = mpw_marshall_read( sitesInputData, sitesInputFormat, importMasterPassword, &marshallError );
                mpw_free_string( &importMasterPassword );
            }
            if (user) {
                mpw_free_string( &user->masterPassword );
                user->masterPassword = strdup( masterPassword );
            }
        }
        mpw_free_string( &sitesInputData );

        if (!user || marshallError.type != MPMarshallSuccess) {
            err( "Couldn't parse configuration file:\n  %s: %s\n", sitesPath, marshallError.description );
            mpw_marshal_free( &user );
            mpw_free_string( &sitesPath );
        }
    }
    if (!user)
        user = mpw_marshall_user( fullName, masterPassword, MPAlgorithmVersionCurrent );
    mpw_free_strings( &fullName, &masterPassword, NULL );

    // Load the site object.
    for (size_t s = 0; s < user->sites_count; ++s) {
        site = &user->sites[s];
        if (strcmp( siteName, site->name ) == 0)
            // Found.
            break;
        site = NULL;
    }
    if (!site)
        site = mpw_marshall_site( user, siteName, MPResultTypeDefault, MPCounterValueDefault, user->algorithm );
    mpw_free_string( &siteName );

    // Load the question object.
    switch (keyPurpose) {
        case MPKeyPurposeAuthentication:
        case MPKeyPurposeIdentification:
            break;
        case MPKeyPurposeRecovery:
            for (size_t q = 0; q < site->questions_count; ++q) {
                question = &site->questions[q];
                if ((!keyContext && !strlen( question->keyword )) ||
                    (keyContext && strcmp( keyContext, question->keyword ) != 0))
                    // Found.
                    break;
                question = NULL;
            }
            if (!question)
                question = mpw_marshal_question( site, keyContext );
            break;
    }

    // Initialize purpose-specific operation parameters.
    MPResultType resultType = MPResultTypeDefault;
    MPCounterValue siteCounter = MPCounterValueDefault;
    const char *purposeResult = NULL, *resultState = NULL;
    switch (keyPurpose) {
        case MPKeyPurposeAuthentication: {
            purposeResult = "password";
            resultType = site->type;
            resultState = site->content? strdup( site->content ): NULL;
            siteCounter = site->counter;
            break;
        }
        case MPKeyPurposeIdentification: {
            purposeResult = "login";
            resultType = site->loginType;
            resultState = site->loginContent? strdup( site->loginContent ): NULL;
            siteCounter = MPCounterValueInitial;
            break;
        }
        case MPKeyPurposeRecovery: {
            mpw_free_string( &keyContext );
            purposeResult = "answer";
            keyContext = question->keyword? strdup( question->keyword ): NULL;
            resultType = question->type;
            resultState = question->content? strdup( question->content ): NULL;
            siteCounter = MPCounterValueInitial;
            break;
        }
    }

    // Override operation parameters from command-line arguments.
    if (resultTypeArg) {
        resultType = mpw_typeWithName( resultTypeArg );
        if (ERR == (int)resultType) {
            ftl( "Invalid type: %s\n", resultTypeArg );
            mpw_marshal_free( &user );
            mpw_free_strings( &sitesPath, &resultState, &keyContext, NULL );
            return EX_USAGE;
        }

        if (!(resultType & MPSiteFeatureAlternative)) {
            switch (keyPurpose) {
                case MPKeyPurposeAuthentication:
                    site->type = resultType;
                    break;
                case MPKeyPurposeIdentification:
                    site->loginType = resultType;
                    break;
                case MPKeyPurposeRecovery:
                    question->type = resultType;
                    break;
            }
        }
    }
    if (siteCounterArg) {
        long long int siteCounterInt = atoll( siteCounterArg );
        if (siteCounterInt < MPCounterValueFirst || siteCounterInt > MPCounterValueLast) {
            ftl( "Invalid site counter: %s\n", siteCounterArg );
            mpw_marshal_free( &user );
            mpw_free_strings( &sitesPath, &resultState, &keyContext, NULL );
            return EX_USAGE;
        }

        switch (keyPurpose) {
            case MPKeyPurposeAuthentication:
                siteCounter = site->counter = (MPCounterValue)siteCounterInt;
                break;
            case MPKeyPurposeIdentification:
            case MPKeyPurposeRecovery:
                // NOTE: counter for login & question is not persisted.
                break;
        }
    }
    const char *resultParam = NULL;
    if (resultParamArg)
        resultParam = strdup( resultParamArg );
    if (algorithmVersionArg) {
        int algorithmVersionInt = atoi( algorithmVersionArg );
        if (algorithmVersionInt < MPAlgorithmVersionFirst || algorithmVersionInt > MPAlgorithmVersionLast) {
            ftl( "Invalid algorithm version: %s\n", algorithmVersionArg );
            mpw_marshal_free( &user );
            mpw_free_strings( &sitesPath, &resultState, &keyContext, &resultParam, NULL );
            return EX_USAGE;
        }
        site->algorithm = (MPAlgorithmVersion)algorithmVersionInt;
    }
    if (sitesRedactedArg)
        user->redacted = strcmp( sitesRedactedArg, "1" ) == 0;
    else if (!user->redacted)
        wrn( "Sites configuration is not redacted.  Use -R 1 to change this.\n" );
    mpw_free_strings( &fullNameArg, &masterPasswordArg, &siteNameArg, NULL );
    mpw_free_strings( &resultTypeArg, &resultParamArg, &siteCounterArg, &algorithmVersionArg, NULL );
    mpw_free_strings( &keyPurposeArg, &keyContextArg, &sitesFormatArg, &sitesRedactedArg, NULL );

    // Operation summary.
    const char *identicon = mpw_identicon( user->fullName, user->masterPassword );
    if (!identicon)
        wrn( "Couldn't determine identicon.\n" );
    dbg( "-----------------\n" );
    dbg( "fullName         : %s\n", user->fullName );
    trc( "masterPassword   : %s\n", user->masterPassword );
    dbg( "identicon        : %s\n", identicon );
    dbg( "sitesFormat      : %s%s\n", mpw_nameForFormat( sitesFormat ), sitesFormatFixed? " (fixed)": "" );
    dbg( "sitesPath        : %s\n", sitesPath );
    dbg( "siteName         : %s\n", site->name );
    dbg( "siteCounter      : %u\n", siteCounter );
    dbg( "resultType       : %s (%u)\n", mpw_nameForType( resultType ), resultType );
    dbg( "resultParam      : %s\n", resultParam );
    dbg( "keyPurpose       : %s (%u)\n", mpw_nameForPurpose( keyPurpose ), keyPurpose );
    dbg( "keyContext       : %s\n", keyContext );
    dbg( "algorithmVersion : %u\n", site->algorithm );
    dbg( "-----------------\n\n" );
    inf( "%s's %s for %s:\n[ %s ]: ", user->fullName, purposeResult, site->name, identicon );
    mpw_free_strings( &identicon, &sitesPath, NULL );

    // Determine master key.
    MPMasterKey masterKey = mpw_masterKey(
            user->fullName, user->masterPassword, site->algorithm );
    if (!masterKey) {
        ftl( "Couldn't derive master key.\n" );
        mpw_marshal_free( &user );
        mpw_free_strings( &resultState, &keyContext, &resultParam, NULL );
        return EX_SOFTWARE;
    }

    // Update state.
    if (resultParam && resultType & MPResultTypeClassStateful) {
        mpw_free_string( &resultState );
        if (!(resultState = mpw_siteState( masterKey, site->name, siteCounter,
                keyPurpose, keyContext, resultType, resultParam, site->algorithm ))) {
            ftl( "Couldn't encrypt site result.\n" );
            mpw_free( &masterKey, MPMasterKeySize );
            mpw_marshal_free( &user );
            mpw_free_strings( &resultState, &keyContext, &resultParam, NULL );
            return EX_SOFTWARE;
        }
        inf( "(state) %s => ", resultState );

        switch (keyPurpose) {
            case MPKeyPurposeAuthentication: {
                mpw_free_string( &site->content );
                site->content = strdup( resultState );
                break;
            }
            case MPKeyPurposeIdentification: {
                mpw_free_string( &site->loginContent );
                site->loginContent = strdup( resultState );
                break;
            }

            case MPKeyPurposeRecovery: {
                mpw_free_string( &question->content );
                question->content = strdup( resultState );
                break;
            }
        }

        // resultParam is consumed.
        mpw_free_string( &resultParam );
    }

    // Second phase resultParam defaults to state.
    if (!resultParam && resultState)
        resultParam = strdup( resultState );
    mpw_free_string( &resultState );

    // Generate result.
    const char *result = mpw_siteResult( masterKey, site->name, siteCounter,
            keyPurpose, keyContext, resultType, resultParam, site->algorithm );
    mpw_free( &masterKey, MPMasterKeySize );
    mpw_free_strings( &keyContext, &resultParam, NULL );
    if (!result) {
        ftl( "Couldn't generate site result.\n" );
        mpw_marshal_free( &user );
        return EX_SOFTWARE;
    }
    fprintf( stdout, "%s\n", result );
    if (site->url)
        inf( "See: %s\n", site->url );
    mpw_free_string( &result );

    // Update usage metadata.
    site->lastUsed = user->lastUsed = time( NULL );
    site->uses++;

    // Update the mpsites file.
    if (sitesFormat != MPMarshallFormatNone) {
        if (!sitesFormatFixed)
            sitesFormat = MPMarshallFormatDefault;
        sitesPath = mpw_path( user->fullName, mpw_marshall_format_extension( sitesFormat ) );

        dbg( "Updating: %s (%s)\n", sitesPath, mpw_nameForFormat( sitesFormat ) );
        if (!sitesPath || !mpw_mkdirs( sitesPath ) || !(sitesFile = fopen( sitesPath, "w" )))
            wrn( "Couldn't create updated configuration file:\n  %s: %s\n", sitesPath, strerror( errno ) );

        else {
            char *buf = NULL;
            MPMarshallError marshallError = { .type = MPMarshallSuccess };
            if (!mpw_marshall_write( &buf, sitesFormat, user, &marshallError ) || marshallError.type != MPMarshallSuccess)
                wrn( "Couldn't encode updated configuration file:\n  %s: %s\n", sitesPath, marshallError.description );

            else if (fwrite( buf, sizeof( char ), strlen( buf ), sitesFile ) != strlen( buf ))
                wrn( "Error while writing updated configuration file:\n  %s: %d\n", sitesPath, ferror( sitesFile ) );

            mpw_free_string( &buf );
            fclose( sitesFile );
        }
        mpw_free_string( &sitesPath );
    }
    mpw_marshal_free( &user );

    return 0;
}
