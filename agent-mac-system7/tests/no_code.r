/* A valid resource fork with no executable CODE resources, for verifying
 * that a Process Manager launch failure restores the previous application.
 * Build with: Rez -t APPL -c '????' -o no_code.bin no_code.r
 * Use only with the rollback-capable updater and an independent backup.
 */
data 'TEST' (128) { $"74657374" };
