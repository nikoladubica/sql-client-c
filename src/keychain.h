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

#endif
