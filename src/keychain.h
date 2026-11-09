#ifndef SQLC_KEYCHAIN_H
#define SQLC_KEYCHAIN_H

/*
 * Connection passwords in the macOS login Keychain, as generic passwords
 * with service "sqlc" and the profile name as the account.
 */

/* Adds or replaces the password. Returns 0, or -1 with a message in err. */
int keychain_set(const char *profile, const char *password, char *err, int errlen);

/* malloc'd password, or NULL if there is none (or access was denied). */
char *keychain_get(const char *profile);

/* Removing a missing item is not an error. */
void keychain_delete(const char *profile);

/* The same for any service/account, e.g. the Claude API key
 * (KEYCHAIN_AI_SERVICE / KEYCHAIN_AI_ACCOUNT). */
#define KEYCHAIN_AI_SERVICE "sqlc-ai"
#define KEYCHAIN_AI_ACCOUNT "anthropic-api-key"
int keychain_set_item(const char *service, const char *account, const char *secret,
                      char *err, int errlen);
char *keychain_get_item(const char *service, const char *account);
void keychain_delete_item(const char *service, const char *account);

#endif
