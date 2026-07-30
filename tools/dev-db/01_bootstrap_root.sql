-- Noggit Red dev-database bootstrap. REQUIRES ROOT (or any account with CREATE USER + GRANT).
--
-- The project's `trinity` account holds only USAGE globally, so it cannot run this.
--
-- Before running: replace both __CHANGE_ME__ placeholders with passwords you choose.
-- Do not commit this file after filling them in.
--
--   mysql -u root -p < tools/dev-db/01_bootstrap_root.sql
--
-- What this creates, and why each piece exists:
--
--   noggit_dev_world  the ONLY schema this project may write to. Every emitted changeset is
--                     rehearsed here before a human applies it anywhere real.
--
--   noggit_ro         SELECT-only, scoped to the live `world` schema. This is what the editor
--                     uses to read real spawns. It is defence in depth against the upstream
--                     defect in src/mysql/mysql.cpp, whose connect() issues
--                     `CREATE DATABASE IF NOT EXISTS` on every call: with this grant the
--                     statement is refused by the server rather than relying on our own code
--                     to behave. Privilege enforcement at the DB beats enforcement in the app.
--
--   noggit_rw         Full rights on noggit_dev_world and nowhere else. Used for rehearsing
--                     changesets and for the existing UID-storage table.

CREATE DATABASE IF NOT EXISTS `noggit_dev_world`
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci;

-- Read-only editor account -------------------------------------------------------------
CREATE USER IF NOT EXISTS 'noggit_ro'@'localhost' IDENTIFIED BY '__CHANGE_ME__';
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'noggit_ro'@'localhost';
GRANT SELECT ON `world`.*            TO 'noggit_ro'@'localhost';
GRANT SELECT ON `noggit_dev_world`.* TO 'noggit_ro'@'localhost';

-- Dev-schema read/write account --------------------------------------------------------
CREATE USER IF NOT EXISTS 'noggit_rw'@'localhost' IDENTIFIED BY '__CHANGE_ME__';
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'noggit_rw'@'localhost';
GRANT ALL PRIVILEGES ON `noggit_dev_world`.* TO 'noggit_rw'@'localhost';
GRANT SELECT          ON `world`.*           TO 'noggit_rw'@'localhost';

FLUSH PRIVILEGES;

-- Verify: noggit_ro must show SELECT only, and neither account may hold any privilege on any
-- other live schema on this server -- compare against protectedSchemas in db-policy.json.
SHOW GRANTS FOR 'noggit_ro'@'localhost';
SHOW GRANTS FOR 'noggit_rw'@'localhost';
