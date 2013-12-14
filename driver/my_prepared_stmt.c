/*
  Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.

  The MySQL Connector/ODBC is licensed under the terms of the GPLv2
  <http://www.gnu.org/licenses/old-licenses/gpl-2.0.html>, like most
  MySQL Connectors. There are special exceptions to the terms and
  conditions of the GPLv2 as it is applied to this software, see the
  FLOSS License Exception
  <http://www.mysql.com/about/legal/licensing/foss-exception.html>.
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; version 2 of the License.
  
  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
  for more details.
  
  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/**
  @file  ssps.c
  @brief Functions to support use of Server Side Prepared Statements.
*/

#include "driver.h"



/* {{{ my_l_to_a() -I- */
static char * my_l_to_a(char * buf, size_t buf_size, long long a)
{
	snprintf(buf, buf_size, "%lld", (long long) a);
	return buf;
}
/* }}} */


/* {{{ my_ul_to_a() -I- */
static char * my_ul_to_a(char * buf, size_t buf_size, unsigned long long a)
{
	snprintf(buf, buf_size, "%llu", (unsigned long long) a);
	return buf;
}
/* }}} */


/* {{{ my_f_to_a() -I- */
static char * my_f_to_a(char * buf, size_t buf_size, double a)
{
	snprintf(buf, buf_size, "%f", a);
	return buf;
}
/* }}} */


void ssps_init(STMT *stmt)
{
  
  stmt->ssps= mysql_stmt_init(&stmt->dbc->mysql);

  if (stmt->dbc->ds->cursor_prefetch_number > 0)
  {
    unsigned long type= (unsigned long) CURSOR_TYPE_READ_ONLY;

    /* TODO check return value */
    mysql_stmt_attr_set(stmt->ssps, STMT_ATTR_CURSOR_TYPE, (void*) &type);
    mysql_stmt_attr_set(stmt->ssps, STMT_ATTR_PREFETCH_ROWS,
                      (void*) &stmt->dbc->ds->cursor_prefetch_number);
  }

  stmt->result_bind= 0;
}


static void free_result_bind(STMT *stmt)
{
  if (stmt->result_bind != NULL)
  {
    x_free(stmt->result_bind[0].is_null);
    x_free(stmt->result_bind[0].length);
    x_free(stmt->result_bind[0].error);
    x_free(stmt->result_bind[0].buffer);

    x_free(stmt->result_bind);
    stmt->result_bind= 0;

    x_free(stmt->array);
    stmt->array= 0;
  }
}


void ssps_close(STMT *stmt)
{
  if (stmt->ssps != NULL)
  {
    mysql_stmt_close(stmt->ssps);
    stmt->ssps= NULL;

    free_result_bind(stmt);
  }
}


/* The structure and following allocation function are borrowed from c/c++ and adopted */
typedef struct tagBST
{  char * buffer;
  size_t size;
  enum enum_field_types type;
} st_buffer_size_type;


/* {{{ allocate_buffer_for_field() -I- */
static st_buffer_size_type
allocate_buffer_for_field(const MYSQL_FIELD * const field)
{
  st_buffer_size_type result= {NULL, 0, field->type};

  switch (field->type) {
    case MYSQL_TYPE_NULL:
      break;

    case MYSQL_TYPE_TINY:
      result.size= 1;
      break;

    case MYSQL_TYPE_SHORT:
      result.size=2;
      break;

    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_FLOAT:
      result.size=4;
      break;

    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_LONGLONG:
      result.size=8;
      break;

    case MYSQL_TYPE_YEAR:
      result.size=2;
      break;

    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
      result.size=sizeof(MYSQL_TIME);
      break;

    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
      /* We will get length with fetch and then fetch column */
      if (field->length > 0 && field->length < 1025)
        result.size= field->length + 1;
      break;

    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      result.size=64;
      break;

    #if A1
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_YEAR:
      result.size= 10;
      break;
    #endif
    #if A0
    // There two are not sent over the wire
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    #endif
    case MYSQL_TYPE_BIT:
      result.size= 1;/*8?*/
      break;
    case MYSQL_TYPE_GEOMETRY:
    default:
      /* Error? */
      1;
  }

  if (result.size > 0)
  {
    result.buffer=my_malloc(result.size, MYF(0));
  }

  return result;
}
/* }}} */


static MYSQL_ROW fetch_varlength_columns(STMT *stmt, MYSQL_ROW columns)
{
  const unsigned int  num_fields= field_count(stmt);
  unsigned int i;

  for (i= 0; i < num_fields; ++i)
  {
    if (stmt->result_bind[i].buffer == NULL)
    {
      if (stmt->lengths[i] < *stmt->result_bind[i].length)
      {
        /* TODO Realloc error proc */
        stmt->array[i] = my_realloc(stmt->array[i], *stmt->result_bind[i].length,
          MYF(MY_ALLOW_ZERO_PTR));
        stmt->lengths[i]= *stmt->result_bind[i].length;
      }

      stmt->result_bind[i].buffer= stmt->array[i];
      stmt->result_bind[i].buffer_length= stmt->lengths[i];

      mysql_stmt_fetch_column(stmt->ssps, stmt->result_bind, i, 0);
    }
  }

  fill_ird_data_lengths(stmt->ird, stmt->result_bind[0].length,
                                  stmt->result->field_count);

  return stmt->array;
}


int ssps_bind_result(STMT *stmt)
{
  const unsigned int  num_fields= field_count(stmt);
  unsigned int        i;

  if (stmt->result_bind)
  {
    /* We have fields requiring to read real length first */
    if (stmt->fix_fields != NULL)
    {
      for (i=0; i < num_fields; ++i)
      {
        /* length marks such fields */
        if (stmt->lengths[i] > 0)
        {
          /* Resetting buffer and buffer_length for those fields */
          stmt->result_bind[i].buffer       = 0;
          stmt->result_bind[i].buffer_length= 0;
        }
      }
    }
  }
  else
  {
    my_bool             *is_null= my_malloc(sizeof(my_bool)*num_fields, MYF(MY_ZEROFILL));
    my_bool             *err    = my_malloc(sizeof(my_bool)*num_fields, MYF(MY_ZEROFILL));
    unsigned long       *len= my_malloc(sizeof(unsigned long)*num_fields, MYF(MY_ZEROFILL));

    /*TODO care about memory allocation errors */
    stmt->result_bind=  (MYSQL_BIND*)my_malloc(sizeof(MYSQL_BIND)*num_fields, MYF(0));
    stmt->array=        (MYSQL_ROW)my_malloc(sizeof(char*)*num_fields, MYF(MY_ZEROFILL));

    for (i= 0; i < num_fields; ++i)
    {
      MYSQL_FIELD    *field= mysql_fetch_field(stmt->result);
      st_buffer_size_type p= allocate_buffer_for_field(field);

      stmt->result_bind[i].buffer_type  = p.type;
	  stmt->result_bind[i].buffer       = p.buffer;
	  stmt->result_bind[i].buffer_length= (unsigned long)p.size;
	  stmt->result_bind[i].length       = &len[i];
	  stmt->result_bind[i].is_null      = &is_null[i];
	  stmt->result_bind[i].error        = &err[i];
	  stmt->result_bind[i].is_unsigned  = field->flags & UNSIGNED_FLAG;

      stmt->array[i]= p.buffer;

      /* Marking that there are columns that will require buffer (re) allocating */
      if (  stmt->result_bind[i].buffer       == 0
        &&  stmt->result_bind[i].buffer_type  != MYSQL_TYPE_NULL)
      {
        stmt->fix_fields= fetch_varlength_columns;
        
        stmt->lengths= my_malloc(sizeof(unsigned long)*num_fields, MYF(MY_ZEROFILL));
        /* Buffer of initial length? */
      }
    }
  }

  mysql_stmt_bind_result(stmt->ssps, stmt->result_bind);

  return 0;
}


BOOL ssps_0buffers_truncated_only(STMT *stmt)
{
  if (stmt->fix_fields == NULL)
  {
    /* That is enough to tell that not */
    return FALSE;
  }
  else
  {
    const unsigned int  num_fields= field_count(stmt);
    unsigned int i;

    for (i= 0; i < num_fields; ++i)
    {
      if (*stmt->result_bind[i].error != 0
        && stmt->result_bind[i].buffer_length > 0
        && stmt->result_bind[i].buffer != NULL)
      {
        return FALSE;
      }
    }
  }

  return TRUE;
}


/* --------------- Type conversion functions -------------- */

#define ALLOC_IFNULL(buff,size) ((buff)==NULL?(char*)my_malloc(size,MYF(0)):buff)

/* {{{ Prepared ResultSet ssps_get_string() -I- */
/* caller should care to make buffer long enough,
   if buffer is not null function allocates memory and that is caller's duty to clean it
 */
char * ssps_get_string(STMT *stmt, ulong column_number, char *value, ulong *length,
                       char * buffer)
{
  MYSQL_BIND *col_rbind= &stmt->result_bind[column_number];

  if (*col_rbind->is_null)
  {
    return NULL;
  }

  switch (col_rbind->buffer_type)
  {
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME:
    {
      MYSQL_TIME * t = (MYSQL_TIME *)(col_rbind->buffer);

      buffer= ALLOC_IFNULL(buffer, 30);
      snprintf(buffer, 20, "%04u-%02u-%02u %02u:%02u:%02u",
                      t->year, t->month, t->day, t->hour, t->minute, t->second);

      *length= 19;

      if (t->second_part > 0)
      {
        snprintf(buffer+*length, 8, ".%06lu", t->second_part);
        *length= 26;
      }

      return buffer;
    }
    case MYSQL_TYPE_DATE:
    {
      MYSQL_TIME * t = (MYSQL_TIME *)(col_rbind->buffer);

      buffer= ALLOC_IFNULL(buffer, 12);
      snprintf(buffer, 11, "%04u-%02u-%02u", t->year, t->month, t->day);
      *length= 10;

      return buffer;
    }
    case MYSQL_TYPE_TIME:
    {
      MYSQL_TIME * t = (MYSQL_TIME *)(col_rbind->buffer);

      buffer= ALLOC_IFNULL(buffer, 20);
      snprintf(buffer, 10, "%s%02u:%02u:%02u", t->neg? "-":"", t->hour,
                                              t->minute, t->second);
      *length= t->neg ? 9 : 8;

      if (t->second_part > 0)
      {
        snprintf(buffer+*length, 8, ".%06lu", t->second_part);
        *length+= 7;
      }
      return buffer;
    }
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_YEAR:  // fetched as a SMALLINT
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    {
      buffer= ALLOC_IFNULL(buffer, 30);

      if (col_rbind->is_unsigned)
      {
        my_ul_to_a(buffer, 29,
          (unsigned long long)ssps_get_int64(stmt, column_number, value, *length));
      }
      else
      {
        my_l_to_a(buffer, 29,
                  ssps_get_int64(stmt, column_number, value, *length));
      }

      *length= strlen(buffer);
      return buffer;
    }
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    {
      buffer= ALLOC_IFNULL(buffer, 50);
      my_f_to_a(buffer, 49, ssps_get_double(stmt, column_number, value,
                                            *length));

      *length= strlen(buffer);
      return buffer;
    }

    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
      return (char *)(col_rbind->buffer);
    default:
      break;
    // TODO : Geometry? default ?
  }

  /* Basically should be prevented by earlied tests of
    conversion possibility */
  return NULL;
}
/* }}} */


long double ssps_get_double(STMT *stmt, ulong column_number, char *value, ulong length)
{
  MYSQL_BIND *col_rbind= &stmt->result_bind[column_number];

  if (*col_rbind->is_null)
  {
    return 0.0;
  }

  switch (col_rbind->buffer_type) {
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_YEAR:  // fetched as a SMALLINT
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    {
      long double ret;
      BOOL is_it_unsigned = col_rbind->is_unsigned != 0;

      if (is_it_unsigned)
      {
        unsigned long long ival = (unsigned long long)ssps_get_int64(stmt, column_number, value, length);
        ret = (long double)(ival);
      }
      else
      {
        long long ival = ssps_get_int64(stmt, column_number, value, length);
        ret = (long double)(ival);
      }

      return ret;
    }
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    {
      char buf[50];
      long double ret = strtold(ssps_get_string(stmt, column_number, value,
                                                &length, buf), NULL);
      return ret;
    }

    case MYSQL_TYPE_FLOAT:
    {
      long double ret = !*col_rbind->is_null? *(float *)(col_rbind->buffer):0.;
      return ret;
    }

    case MYSQL_TYPE_DOUBLE:
    {
      long double ret = !*col_rbind->is_null? *(double *)(col_rbind->buffer):0.;
      return ret;
    }

    /* TODO : Geometry? default ? */
  }
  
  /* Basically should be prevented by earlied tests of
     conversion possibility */
  return .0;
}


/* {{{ Prepared ResultSet ssps_get_int64() -I- */
long long ssps_get_int64(STMT *stmt, ulong column_number, char *value, ulong length)
{
  MYSQL_BIND *col_rbind= &stmt->result_bind[column_number];

  switch (col_rbind->buffer_type)
  {
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:

      return (long long)ssps_get_double(stmt, column_number, value, length);

    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    {
      char buf[30];
      return strtoll(ssps_get_string(stmt, column_number, value, &length, buf),
                      NULL, 10);
    }
    case MYSQL_TYPE_BIT:
    {
      long long uval = 0;
      /* This length is in bytes, on the contrary to what can be seen in mysql_resultset.cpp where the Meta is used */
      return binary2numeric(&uval, col_rbind->buffer, *col_rbind->length);
    }

    case MYSQL_TYPE_YEAR:  // fetched as a SMALLINT
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    {
      // MYSQL_TYPE_YEAR is fetched as a SMALLINT, thus should not be in the switch
      long long ret;
      BOOL is_it_null = *col_rbind->is_null != 0;
      BOOL is_it_unsigned = col_rbind->is_unsigned != 0;

      switch (col_rbind->buffer_length)
      {
        case 1:
          if (is_it_unsigned)
          {
            ret = !is_it_null? ((char *)col_rbind->buffer)[0]:0;
          }
          else
          {
            ret = !is_it_null? *(char *)(col_rbind->buffer):0;
          }
          break;

        case 2:

          if (is_it_unsigned)
          {
            ret = !is_it_null? *(unsigned short *)(col_rbind->buffer):0;
          }
          else
          {
            ret = !is_it_null? *(unsigned short *)(col_rbind->buffer):0;
          }
          break;

        case 4:

          if (is_it_unsigned)
          {
            ret =  !is_it_null? *(int *)(col_rbind->buffer):0;
          }
          else
          {
            ret =  !is_it_null? *(unsigned int *)(col_rbind->buffer):0;
          }
          break;

        case 8:

          if (is_it_unsigned)
          {
            ret =  !is_it_null? *(unsigned long long *)col_rbind->buffer:0;

#if WE_WANT_TO_SEE_MORE_FAILURES_IN_UNIT_RESULTSET
            if (cutTooBig &&
              ret &&
              *(unsigned long long *)(col_rbind->buffer) > UL64(9223372036854775807))
            {
              ret = UL64(9223372036854775807);
            }
#endif
          }
          else
          {
            ret =  !is_it_null? *(long long *)(col_rbind->buffer):0;
          }
          break;
        default:
          return 0; 
      }
      return ret;
    }
    default:
      break;/* Basically should be prevented by earlied tests of
                       conversion possibility */
    /* TODO : Geometry? default ? */
  }

  return 0; // fool compilers
}
/* }}} */
