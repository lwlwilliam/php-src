--TEST--
DatePeriod::INCLUDE_END_DATE
--FILE--
<?php
date_default_timezone_set('UTC');
$start = new DateTime('2010-06-07');
$end = new DateTime('2010-06-10');
$interval = new DateInterval('P1D');

foreach (new DatePeriod($start, $interval, $end, DatePeriod::INCLUDE_END_DATE) as $day) {
    echo $day->format('Y-m-d') . "\n";
}
?>
--EXPECT--
2010-06-07
2010-06-08
2010-06-09
2010-06-10
