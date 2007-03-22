. support_funcs.sh

init_dml()
{
  echo "init_dml()"
}

begin()
{
  echo "begin()"
}

rollback()
{
  echo "rollback()"
}

commit()
{
  echo "commit()"
}

generate_initdata()
{
  numrows=$(random_number 50 1000)
  i=0;
  trippoint=`expr $numrows / 20`
  j=0;
  percent=0
  status "generating ${numrows} transactions of random data"
  percent=`expr $j \* 5`
  status "$percent %"
  GENDATA="$mktmp/generate.data"
  while : ; do
    txtalen=$(random_number 1 100)
    txta=$(random_string ${txtalen})
    txta=`echo ${txta} | sed -e "s/\\\\\\\/\\\\\\\\\\\\\\/g" -e "s/'/''/g"`
    txtblen=$(random_number 1 100)
    txtb=$(random_string ${txtblen})
    txtb=`echo ${txtb} | sed -e "s/\\\\\\\/\\\\\\\\\\\\\\/g" -e "s/'/''/g"`
    txtclen=$(random_number 1 100)
    txtc=$(random_string ${txtclen})
    txtc=`echo ${txtc} | sed -e "s/\\\\\\\/\\\\\\\\\\\\\\/g" -e "s/'/''/g"`
    txtdlen=$(random_number 1 100)
    txtd=$(random_string ${txtdlen})
    txtd=`echo ${txtd} | sed -e "s/\\\\\\\/\\\\\\\\\\\\\\/g" -e "s/'/''/g"`
    echo "INSERT INTO sub1(data) VALUES ('sub1 ${txta}');" >> $GENDATA
    echo "INSERT INTO sub2(data) VALUES ('sub2 ${txtb}');" >> $GENDATA
    echo "INSERT INTO sub3(data) VALUES ('sub3 ${txtc}');" >> $GENDATA
    echo "INSERT INTO master(data) VALUES ('master ${txtd}');" >> $GENDATA
    echo "select purchase_product( region_code, product_id, (random()*5+random()*8+random()*7)::integer) from regions, products order by random() limit 3;" >> $GENDATA
    if [ ${i} -ge ${numrows} ]; then
      break;
    else
      i=$((${i} +1))
      working=`expr $i % $trippoint`
      if [ $working -eq 0 ]; then
        j=`expr $j + 1`
        percent=`expr $j \* 5`
        status "$percent %"
      fi 
    fi
  done
  status "done"
}

do_initdata()
{
  originnode=${ORIGINNODE:-"1"}
  eval db=\$DB${originnode}
  eval host=\$HOST${originnode}
  eval user=\$USER${originnode}
  eval port=\$PORT${originnode}
  generate_initdata
  launch_poll
  status "loading data"
  $pgbindir/psql -h $host -p $port -d $db -U $user < $mktmp/generate.data 1> $mktmp/initdata.log 2> $mktmp/initdata.log
  if [ $? -ne 0 ]; then
    warn 3 "do_initdata failed, see $mktmp/initdata.log for details"
  fi 
  wait_for_catchup
  status "done"
}
