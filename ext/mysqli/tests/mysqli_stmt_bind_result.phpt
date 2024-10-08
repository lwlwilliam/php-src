--TEST--
mysqli_stmt_bind_result()
--EXTENSIONS--
mysqli
--SKIPIF--
<?php
require_once 'skipifconnectfailure.inc';
?>
--FILE--
<?php
    require 'table.inc';

    $stmt = mysqli_stmt_init($link);
    if (!mysqli_stmt_prepare($stmt, "SELECT id, label FROM test ORDER BY id LIMIT 1"))
        printf("[002a] [%d] %s\n", mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));

    mysqli_stmt_close($stmt);
    $stmt = mysqli_stmt_init($link);

    $id = null;
    $label = null;
    $foo = null;

    try {
        mysqli_stmt_bind_result($stmt, $id);
    } catch (Error $exception) {
        echo $exception->getMessage() . "\n";
    }

    if (!mysqli_stmt_prepare($stmt, "SELECT id, label FROM test ORDER BY id LIMIT 1"))
        printf("[004] [%d] %s\n", mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));

    try {
        mysqli_stmt_bind_result($stmt, $id);
    } catch (\ArgumentCountError $e) {
        echo $e->getMessage() . PHP_EOL;
    }

    if (true !== ($tmp = mysqli_stmt_bind_result($stmt, $id, $label)))
        printf("[006] Expecting boolean/true, got %s/%s\n", gettype($tmp), $tmp);

    try {
        mysqli_stmt_bind_result($stmt, $id, $label, $foo);
    } catch (\ArgumentCountError $e) {
        echo $e->getMessage() . PHP_EOL;
    }

    if (!mysqli_stmt_execute($stmt))
        printf("[008] [%d] %s\n", mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));

    while (mysqli_stmt_fetch($stmt)) {
        var_dump($id);
        var_dump($label);
    }
    mysqli_stmt_close($stmt);


    function func_mysqli_stmt_bind_result($link, $engine, $bind_type, $sql_type, $bind_value, $offset, $type_hint = null) {

        if (!mysqli_query($link, "DROP TABLE IF EXISTS test")) {
            printf("[%04d] [%d] %s\n", $offset, mysqli_errno($link), mysqli_error($link));
            return false;
        }

        if (!mysqli_query($link, sprintf("CREATE TABLE test(id INT, label %s, PRIMARY KEY(id)) ENGINE = %s", $sql_type, $engine))) {
            // don't bail - column type might not be supported by the server, ignore this
            return false;
        }

        if (!$stmt = mysqli_stmt_init($link)) {
            printf("[%04d] [%d] %s\n", $offset + 1, mysqli_errno($link), mysqli_error($link));
            return false;
        }

        if (!mysqli_stmt_prepare($stmt, "INSERT INTO test(id, label) VALUES (?, ?)")) {
            printf("[%04d] [%d] %s\n", $offset + 2, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));
            return false;
        }

        $id = null;
        if (!mysqli_stmt_bind_param($stmt, "i" . $bind_type, $id, $bind_value)) {
            printf("[%04d] [%d] %s\n", $offset + 3, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));
            return false;
        }

        for ($id = 1; $id < 4; $id++) {
            if (!mysqli_stmt_execute($stmt)) {
                printf("[%04d] [%d] %s\n", $offset + 3 + $id, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));
                return false;
            }
        }
        mysqli_stmt_close($stmt);

        $stmt = mysqli_stmt_init($link);

        if (!mysqli_stmt_prepare($stmt, "SELECT id, label FROM test")) {
            printf("[%04d] [%d] %s\n", $offset + 7, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));
            return false;
        }

        if (!mysqli_stmt_execute($stmt)) {
            printf("[%04d] [%d] %s\n", $offset + 8, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));
            return false;
        }

        $bind_res = null;
        if (!mysqli_stmt_bind_result($stmt, $id, $bind_res)) {
            printf("[%04d] [%d] %s\n", $offset + 9, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));
            return false;
        }
        $num = 0;

        while (mysqli_stmt_fetch($stmt)) {
            if ($bind_res !== $bind_value && (!$type_hint || ($type_hint !== gettype($bind_res)))) {
                printf("[%04d] [%d] Expecting %s/'%s' [type hint = %s], got %s/'%s'\n",
                    $offset + 10, $num,
                    gettype($bind_value), $bind_value, $type_hint,
                    gettype($bind_res), $bind_res);
                    return false;
            }
            $num++;
        }

        if ($num != 3) {
            printf("[%04d] [%d] %s, expecting 3 results, got only %d results\n",
                $offset + 11, mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt), $num);
            return false;
        }

        return true;
    }


    function func_mysqli_stmt_bind_make_string($len) {

        $ret = '';
        for ($i = 0; $i < $len; $i++)
            $ret .= chr(mt_rand(65, 90));

        return $ret;
    }

    func_mysqli_stmt_bind_result($link, $engine, "i", "TINYINT", -11, 20);
    func_mysqli_stmt_bind_result($link, $engine, "i", "TINYINT", NULL, 40);
    func_mysqli_stmt_bind_result($link, $engine, "i", "TINYINT UNSIGNED", 1, 60);
    func_mysqli_stmt_bind_result($link, $engine, "i", "TINYINT UNSIGNED", NULL, 80);

    func_mysqli_stmt_bind_result($link, $engine, "i", "BOOL", 1, 100);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BOOL", NULL, 120);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BOOLEAN", 0, 140);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BOOLEAN", NULL, 160);

    func_mysqli_stmt_bind_result($link, $engine, "i", "SMALLINT", -32768, 180);
    func_mysqli_stmt_bind_result($link, $engine, "i", "SMALLINT", 32767, 200);
    func_mysqli_stmt_bind_result($link, $engine, "i", "SMALLINT", NULL, 220);
    func_mysqli_stmt_bind_result($link, $engine, "i", "SMALLINT UNSIGNED", 65535, 240);
    func_mysqli_stmt_bind_result($link, $engine, "i", "SMALLINT UNSIGNED", NULL, 260);

    func_mysqli_stmt_bind_result($link, $engine, "d", "MEDIUMINT", -8388608, 280, "integer");
    func_mysqli_stmt_bind_result($link, $engine, "d", "MEDIUMINT", 8388607, 300, "integer");
    func_mysqli_stmt_bind_result($link, $engine, "d", "MEDIUMINT", NULL, 320);
    func_mysqli_stmt_bind_result($link, $engine, "d", "MEDIUMINT UNSIGNED", 16777215, 340, "integer");
    func_mysqli_stmt_bind_result($link, $engine, "d", "MEDIUMINT UNSIGNED", NULL, 360);

    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER", (defined("PHP_INT_MAX")) ? max(-1 * PHP_INT_MAX + 1, -2147483648) : 1, 380);
    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER", -2147483647, 400, "integer");
    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER", (defined("PHP_INT_MAX")) ? min(2147483647, PHP_INT_MAX) : 1, 420);
    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER", NULL, 440);
    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER UNSIGNED", (defined("PHP_INT_MAX")) ? min(4294967295, 2147483647) : 1, 460);
    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER UNSIGNED", 4294967295, 480, (defined("PHP_INT_MAX") && (4294967295 > PHP_INT_MAX)) ? "string" : null);
    func_mysqli_stmt_bind_result($link, $engine, "i", "INTEGER UNSIGNED", NULL, 500);

    /* test is broken too: we bind "integer" but value is a float
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT", -9223372036854775808, 520);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT UNSIGNED", 18446744073709551615, 560);
    */
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT", NULL, 540);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT UNSIGNED", NULL, 580);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT", -1, 1780);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT UNSIGNED", 1, 1800);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT", -1 * PHP_INT_MAX + 1, 1820);
    func_mysqli_stmt_bind_result($link, $engine, "i", "BIGINT UNSIGNED", PHP_INT_MAX, 1840);

    func_mysqli_stmt_bind_result($link, $engine, "d", "FLOAT", -9237.21, 600);
    func_mysqli_stmt_bind_result($link, $engine, "d", "FLOAT", NULL, 620);
    func_mysqli_stmt_bind_result($link, $engine, "d", "FLOAT UNSIGNED", 18467.5, 640);
    func_mysqli_stmt_bind_result($link, $engine, "d", "FLOAT UNSIGNED ", NULL, 660);

    // Yes, we need the temporary variable. The PHP casting will foul us otherwise.
    func_mysqli_stmt_bind_result($link, $engine, "d", "DOUBLE(10,2)", '-99999999.99', 680, "double");
    func_mysqli_stmt_bind_result($link, $engine, "d", "DOUBLE(10,2)", NULL, 700);
    func_mysqli_stmt_bind_result($link, $engine, "d", "DOUBLE(10,2) UNSIGNED", '99999999.99' , 720, "double");
    func_mysqli_stmt_bind_result($link, $engine, "d", "DOUBLE(10,2) UNSIGNED", NULL, 740);
    func_mysqli_stmt_bind_result($link, $engine, "d", "DECIMAL(10,2)", '-99999999.99', 760, "string");
    func_mysqli_stmt_bind_result($link, $engine, "d", "DECIMAL(10,2)", NULL, 780);
    func_mysqli_stmt_bind_result($link, $engine, "d", "DECIMAL(10,2)", '99999999.99', 800, "string");
    func_mysqli_stmt_bind_result($link, $engine, "d", "DECIMAL(10,2)", NULL, 820);

    // don't care about date() strict TZ warnings...
    func_mysqli_stmt_bind_result($link, $engine, "s", "DATE", @date('Y-m-d'), 840);
    func_mysqli_stmt_bind_result($link, $engine, "s", "DATE NOT NULL", @date('Y-m-d'), 860);
    func_mysqli_stmt_bind_result($link, $engine, "s", "DATE", NULL, 880);

    func_mysqli_stmt_bind_result($link, $engine, "s", "DATETIME", @date('Y-m-d H:i:s'), 900);
    func_mysqli_stmt_bind_result($link, $engine, "s", "DATETIME NOT NULL", @date('Y-m-d H:i:s'), 920);
    func_mysqli_stmt_bind_result($link, $engine, "s", "DATETIME", NULL, 940);

    func_mysqli_stmt_bind_result($link, $engine, "s", "TIMESTAMP", @date('Y-m-d H:i:s'), 960);

    func_mysqli_stmt_bind_result($link, $engine, "s", "TIME", @date('H:i:s'), 980);
    func_mysqli_stmt_bind_result($link, $engine, "s", "TIME NOT NULL", @date('H:i:s'), 1000);
    func_mysqli_stmt_bind_result($link, $engine, "s", "TIME", NULL, 1020);

    $tmp = intval(@date('Y'));
    func_mysqli_stmt_bind_result($link, $engine, "s", "YEAR", $tmp, 1040, "string"); // YEAR is a string with implicit display width of 4
    func_mysqli_stmt_bind_result($link, $engine, "s", "YEAR NOT NULL", $tmp, 1060, "string");
    func_mysqli_stmt_bind_result($link, $engine, "s", "YEAR", NULL, 1080);

    $string255 = func_mysqli_stmt_bind_make_string(255);
    func_mysqli_stmt_bind_result($link, $engine, "s", "CHAR(1)", "a", 1110, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "CHAR(255)", $string255, 1120, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "CHAR(1) NOT NULL", "a", 1140, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "CHAR(1)", NULL, 1160);

    $string65k = func_mysqli_stmt_bind_make_string(65535);
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARCHAR(1)", "a", 1180, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARCHAR(255)", $string255, 1200, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARCHAR(65635)", $string65k, 1220, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARCHAR(1) NOT NULL", "a", 1240, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARCHAR(1)", NULL, 1260);

    func_mysqli_stmt_bind_result($link, $engine, "s", "BINARY(1)", "a", 1280);
    func_mysqli_stmt_bind_result($link, $engine, "s", "BINARY(1)", chr(0), 1300);
    func_mysqli_stmt_bind_result($link, $engine, "s", "BINARY(1) NOT NULL", "b", 1320);
    func_mysqli_stmt_bind_result($link, $engine, "s", "BINARY(1)", NULL, 1340);

    func_mysqli_stmt_bind_result($link, $engine, "s", "VARBINARY(1)", "a", 1360);
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARBINARY(1)", chr(0), 1380);
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARBINARY(1) NOT NULL", "b", 1400);
    func_mysqli_stmt_bind_result($link, $engine, "s", "VARBINARY(1)", NULL, 1420);

    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYBLOB", "a", 1440);
    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYBLOB", chr(0), 1460);
    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYBLOB NOT NULL", "b", 1480);
    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYBLOB", NULL, 1500);

    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYTEXT", "a", 1520, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYTEXT NOT NULL", "a", 1540, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "TINYTEXT", NULL, 1560, 'string');

    // Note: you cannot insert any blob values this way. But you can check the API at least partly this way
    // Extra BLOB tests are in mysqli_stmt_send_long()
    func_mysqli_stmt_bind_result($link, $engine, "b", "BLOB", "", 1580);
    func_mysqli_stmt_bind_result($link, $engine, "b", "TEXT", "", 1600, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "b", "MEDIUMBLOB", "", 1620);
    func_mysqli_stmt_bind_result($link, $engine, "b", "MEDIUMTEXT", "", 1640, 'string');

    /* Is this one related? http://bugs.php.net/bug.php?id=35759 */
    func_mysqli_stmt_bind_result($link, $engine, "b", "LONGBLOB", "", 1660);
    func_mysqli_stmt_bind_result($link, $engine, "b", "LONGTEXT", "", 1680, 'string');

    func_mysqli_stmt_bind_result($link, $engine, "s", "ENUM('a', 'b')", "a", 1700, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "ENUM('a', 'b')", NULL, 1720, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "SET('a', 'b')", "a", 1740, 'string');
    func_mysqli_stmt_bind_result($link, $engine, "s", "SET('a', 'b')", NULL, 1760, 'string');

    if (mysqli_get_server_version($link) >= 50600)
        func_mysqli_stmt_bind_result($link, $engine, "s", "TIME(6)", "13:31:34.123456", 1770);

    $stmt = mysqli_stmt_init($link);
    if (!mysqli_stmt_prepare($stmt, "INSERT INTO test(id, label) VALUES (1000, 'z')"))
        printf("[3001] [%d] %s\n", mysqli_stmt_errno($stmt), mysqli_stmt_error($stmt));

    $id = null;
    try {
        mysqli_stmt_bind_result($stmt, $id);
    } catch (\ArgumentCountError $e) {
        echo $e->getMessage() . \PHP_EOL;
    }

    mysqli_stmt_close($stmt);

    mysqli_close($link);
    print "done!";
?>
--CLEAN--
<?php
    require_once 'clean_table.inc';
?>
--EXPECTF--
mysqli_stmt object is not fully initialized
Number of bind variables doesn't match number of fields in prepared statement
Number of bind variables doesn't match number of fields in prepared statement
int(1)
%s(1) "a"
Number of bind variables doesn't match number of fields in prepared statement
done!
