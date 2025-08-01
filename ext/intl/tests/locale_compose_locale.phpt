--TEST--
locale_compose_locale()
--EXTENSIONS--
intl
--FILE--
<?php

/*
 * Try parsing different Locales
 * with Procedural and Object methods.
 */

function ut_main()
{
    $loc_parts_arr1 = array(
        Locale::LANG_TAG => 'sl',
        Locale::SCRIPT_TAG => 'Latn',
        Locale::REGION_TAG => 'IT'
    );
    $loc_parts_arr2 = array(
        Locale::LANG_TAG => 'de',
        Locale::REGION_TAG => 'DE'
    );
    $loc_parts_arr3 = array(
        Locale::LANG_TAG => 'hi'
    );
    $loc_parts_arr4 = array(
        Locale::LANG_TAG => 'zh',
        Locale::SCRIPT_TAG => 'Hans',
        Locale::REGION_TAG => 'CN'
    );
    $loc_parts_arr5 = array(
        Locale::LANG_TAG => 'es',
        Locale::SCRIPT_TAG => 'Hans',
        Locale::REGION_TAG => 'CN'
    );
    $loc_parts_arr6 = array(
        Locale::LANG_TAG => 'en',
        Locale::SCRIPT_TAG => 'Hans',
        Locale::REGION_TAG => 'CN',
        Locale::VARIANT_TAG.'14' => 'rozaj',
        'variant1' => 'nedis'
    );
    $loc_parts_arr7 = array(
        Locale::LANG_TAG => 'en',
        Locale::SCRIPT_TAG => 'Hans',
        Locale::REGION_TAG => 'CN',
        'variant14' => 'rozaj',
        'variant1' => 'nedis',
        'extlang0' => 'lng',
        'extlang1' => 'ing'
    );
    $loc_parts_arr8 = array(
        Locale::LANG_TAG => 'en',
        Locale::SCRIPT_TAG => 'Hans',
        Locale::REGION_TAG => 'CN',
        'variant14' => 'rozaj',
        'variant1' => 'nedis',
        'extlang0' => 'lng',
        'extlang1' => 'ing',
        'private7' => 'prv1',
        'private9' => 'prv2'
    );
    $loc_parts_arr9 = array(
        Locale::LANG_TAG => 'en',
        Locale::SCRIPT_TAG => 'Hans',
        Locale::REGION_TAG => 'CN',
        Locale::VARIANT_TAG => array('nedis', 'rozaj'),
        Locale::PRIVATE_TAG => array('prv1', 'prv2'),
        Locale::EXTLANG_TAG => array('lng', 'ing')
    );


    $loc_parts_arr = array(
        'loc1' => $loc_parts_arr1,
        'loc2' => $loc_parts_arr2,
        'loc3' => $loc_parts_arr3,
        'loc4' => $loc_parts_arr4,
        'loc5' => $loc_parts_arr5,
        'loc6' => $loc_parts_arr6,
        'loc7' => $loc_parts_arr7,
        'loc8' => $loc_parts_arr8,
        'loc9' => $loc_parts_arr9,
    );

    $cnt  = 0;
    $res_str = '';
    foreach ($loc_parts_arr as $key => $value) {
        $res_str .= "\n------------";
        $res_str .= "\nInput Array name is : loc".(++$cnt) ;

        $locale = ut_loc_locale_compose( $value);
        $res_str .= "\n\nComposed Locale: ";
        if ($locale) {
            $res_str .= "$locale";
        } else {
            $res_str .= "No values found from Locale compose due to the following error:\n";
            $res_str .= intl_get_error_message() ;
        }
    }

    $res_str .= "\n------------";
    $res_str .= "\n";
    return $res_str;

}

include_once( 'ut_common.inc' );
ut_run();

?>
--EXPECT--
------------
Input Array name is : loc1

Composed Locale: sl_Latn_IT
------------
Input Array name is : loc2

Composed Locale: de_DE
------------
Input Array name is : loc3

Composed Locale: hi
------------
Input Array name is : loc4

Composed Locale: zh_Hans_CN
------------
Input Array name is : loc5

Composed Locale: es_Hans_CN
------------
Input Array name is : loc6

Composed Locale: en_Hans_CN_nedis_rozaj
------------
Input Array name is : loc7

Composed Locale: en_lng_ing_Hans_CN_nedis_rozaj
------------
Input Array name is : loc8

Composed Locale: en_lng_ing_Hans_CN_nedis_rozaj_x_prv1_prv2
------------
Input Array name is : loc9

Composed Locale: en_lng_ing_Hans_CN_nedis_rozaj_x_prv1_prv2
------------
