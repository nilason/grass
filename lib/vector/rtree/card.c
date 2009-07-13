
/****************************************************************************
* MODULE:       R-Tree library 
*              
* AUTHOR(S):    Antonin Guttman - original code
*               Daniel Green (green@superliminal.com) - major clean-up
*                               and implementation of bounding spheres
*               Markus Metz - R*-tree
*               
* PURPOSE:      Multidimensional index
*
* COPYRIGHT:    (C) 2009 by the GRASS Development Team
*
*               This program is free software under the GNU General Public
*               License (>=v2). Read the file COPYING that comes with GRASS
*               for details.
*****************************************************************************/

#include "index.h"
#include "card.h"

int NODECARD = MAXCARD;
int LEAFCARD = MAXCARD;

static int set_max(int *which, int new_max)
{
    if (2 > new_max || new_max > MAXCARD)
	return 0;
    *which = new_max;
    return 1;
}

int RTreeSetNodeMax(int new_max, struct RTree *t)
{
    return set_max(&(t->nodecard), new_max);
}
int RTreeSetLeafMax(int new_max, struct RTree *t)
{
    return set_max(&(t->leafcard), new_max);
}
int RTreeGetNodeMax(struct RTree *t)
{
    return t->nodecard;
}
int RTreeGetLeafMax(struct RTree *t)
{
    return t->leafcard;
}
