/****************************************************************
 *
 * MODULE:     v.example
 *
 * AUTHOR(S):  GRASS Development Team, Radim Blazek, Maris Nartiss
 *
 * PURPOSE:    copies vector data from source map to destination map
 *             prints out all point coordinates and atributes
 *
 * COPYRIGHT:  (C) 2002-2008 by the GRASS Development Team
 *
 *             This program is free software under the
 *             GNU General Public License (>=v2).
 *             Read the file COPYING that comes with GRASS
 *             for details.
 *
 ****************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <grass/gis.h>
#include <grass/Vect.h>
#include <grass/dbmi.h>
#include <grass/glocale.h>

int main(int argc, char *argv[])
{
    struct Map_info In, Out;
    static struct line_pnts *Points;
    struct line_cats *Cats;
    int i, type, cat, ncols, nrows, col, more, open3d;
    char *mapset, sql[200];
    struct GModule *module;     /* GRASS module for parsing arguments */
    struct Option *old, *new;
    dbDriver *driver;
    dbHandle handle;
    dbCursor cursor;
    dbTable  *table;
    dbColumn *column;
    dbString table_name, dbsql, valstr;
    struct field_info *Fi, *Fin;

    /* initialize GIS environment */
    /* reads grass env, stores program name to G_program_name() */
    G_gisinit(argv[0]);

    /* initialize module */
    module = G_define_module();
    module->keywords = _("vector, keyword2, keyword3");
    module->description = _("My first vector module");

    /* Define the different options as defined in gis.h */
    old = G_define_standard_option(G_OPT_V_INPUT);

    new = G_define_standard_option(G_OPT_V_OUTPUT);

    /* options and flags parser */
    if (G_parser(argc, argv))
		exit(EXIT_FAILURE);

    /* Create and initialize struct's where to store points/lines and categories */
    Points = Vect_new_line_struct();
    Cats = Vect_new_cats_struct();

	/* Check 1) output is legal vector name; 2) if can find input map; 
		3) if input was found in current mapset, check if input != output.
		lib/vector/Vlib/legal_vname.c
	*/
    Vect_check_input_output_name(old->answer, new->answer, GV_FATAL_EXIT);

    if ((mapset = G_find_vector2(old->answer, "")) == NULL)
		G_fatal_error(_("Vector map <%s> not found"), old->answer);

    /* Predetermine level at which a map will be opened for reading 
    	lib/vector/Vlib/open.c
    */
	if (Vect_set_open_level(2))
		G_fatal_error(_("Unable to set predetermined vector open level"));

    /* Open existing vector for reading
    	lib/vector/Vlib/open.c
    */
    if (1 > Vect_open_old(&In, old->answer, mapset))
		G_fatal_error(_("Unable to open vector map <%s>"), old->answer);
	
	/* Check if old vector is 3D. We should preserve 3D data. */
	if (Vect_is_3d(&In)) open3d = WITH_Z;
	else open3d = WITHOUT_Z;
	
	/* Open new vector for reading/writing */
	if (0 > Vect_open_new(&Out, new->answer, open3d)) {
		Vect_close(&In);
		G_fatal_error(_("Unable to create vector map <%s>"), new->answer);
	}

	/* Let's get vector layers db connections information */
	Fi = Vect_get_field(&In, 1);
	if (!Fi) {
		Vect_close(&In);
		G_fatal_error(_("Database connection not defined for layer %d"), 1);
	}
	/* Output information usefull for debuging 
		incluse/vect/dig_structs.h
	*/
	G_debug(1,"Field number:%d; Name:<%s>; Driver:<%s>; Database:<%s>; Table:<%s>; Key:<%s>;\n",
		Fi->number, Fi->name, Fi->driver, Fi->database, Fi->table, Fi->key);
	
	/* Prepeare strings for use in db_* calls */
	db_init_string(&dbsql);
	db_init_string(&valstr);
	db_init_string(&table_name);
	db_init_handle(&handle);
	
	/* Prepearing database for use */
	driver = db_start_driver(Fi->driver);
	if (driver == NULL) {
		Vect_close(&In);
		G_fatal_error(_("Unable to start driver <%s>"), Fi->driver);
	}
	db_set_handle(&handle, Fi->database, NULL);
	if (db_open_database(driver, &handle) != DB_OK) {
		Vect_close(&In);
		G_fatal_error(_("Unable to open database <%s> by driver <%s>"), Fi->database, driver);
	}
	db_set_string(&table_name, Fi->table);
	if (db_describe_table (driver, &table_name, &table) != DB_OK) {
		Vect_close(&In);
		G_fatal_error (_("Unable to describe table <%s>"), Fi->table);
	}
	ncols = db_get_table_number_of_columns(table);

	/* Copy header and history data from old to new map */
	Vect_copy_head_data(&In, &Out);
	Vect_hist_copy(&In, &Out);
	Vect_hist_command(&Out);

	i = 1;
	/* Let's do something with every vector feature in input map... */
	/* Read in single line and get it's type */
	while ((type = Vect_read_next_line(&In, Points, Cats)) > 0) {
		/* If Points contain line... */
		if (type == GV_LINE || type == GV_POINT || type == GV_CENTROID) {
			if (Vect_cat_get(Cats, 1, &cat) == 0) {
				Vect_cat_set(Cats, 1, i);
				i++;
			}
		}
		if (type == GV_POINT) {
			/* Print out point coordinates */
			printf("No:%d\tX:%f\tY:%f\tZ:%f\tCAT:%d\n",i,*Points->x,*Points->y,*Points->z,cat);
			
			/* Prepeare SQL query to get point atribute data */
			sprintf(sql,"select * from %s where %s=%d",Fi->table,Fi->key,cat);
			G_debug(1,"SQL: \"%s\"",sql);
			db_set_string(&dbsql, sql);
			/* Now execute query */
			if (db_open_select_cursor(driver, &dbsql, &cursor, DB_SEQUENTIAL) != DB_OK)
				G_warning(_("Unabale to get attribute data for cat %d"), cat);
			else {
				/* Result count */
				nrows = db_get_num_rows(&cursor);
				G_debug(1,"Result count: %d",nrows);
				table = db_get_cursor_table (&cursor);
				/* Let's output every columns name and value */
				while(1) {
					if (db_fetch (&cursor, DB_NEXT, &more) != DB_OK) {
						G_warning(_("Error while retreiving database record for cat %d"), cat);
						break;
					}
					if (!more) break;
					for (col = 0; col < ncols; col++) {
						column = db_get_table_column(table, col);
						db_convert_column_value_to_string(column, &valstr);
						printf("%s: %s\t", db_get_column_name(column), db_get_string(&valstr));
					}
					printf("\n");
				}
				db_close_cursor(&cursor);
			}
		}
		/* Only now we write data into new vector */
		Vect_write_line(&Out, type, Points, Cats);
	}
	
	/* Create database for new vector map */
	Fin = Vect_default_field_info(&Out, 1, NULL, GV_1TABLE);
	driver = db_start_driver_open_database(Fin->driver, Fin->database);
	G_debug(1,"Field number:%d; Name:<%s>; Driver:<%s>; Database:<%s>; Table:<%s>; Key:<%s>;\n",
		Fin->number, Fin->name, Fin->driver, Fin->database, Fin->table, Fin->key);
	
	/* Let's copy atribute table data */
	if (db_copy_table(Fi->driver,Fi->database,Fi->table,
	Fin->driver,Vect_subst_var(Fin->database,&Out),Fin->table) == DB_FAILED)
		G_warning(_("Unable to copy atribute table to vector map <%s>"), new->answer);
	else Vect_map_add_dblink(&Out, Fin->number, Fin->name, Fin->table, Fi->key, Fin->database, Fin->driver);
	
    /* Build topology for vector map and close them */
    Vect_build(&Out, stdout);
    Vect_close(&In);
    Vect_close(&Out);
    db_close_database_shutdown_driver(driver);

	/* Don't forget to report to caller sucessfull end of data processing :) */
    exit(EXIT_SUCCESS);
}
