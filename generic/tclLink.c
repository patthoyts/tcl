/* 
 * tclLink.c --
 *
 *	This file implements linked variables (a C variable that is
 *	tied to a Tcl variable).  The idea of linked variables was
 *	first suggested by Andreas Stolcke and this implementation is
 *	based heavily on a prototype implementation provided by
 *	him.
 *
 * Copyright (c) 1993 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclLink.c,v 1.6 2002/02/28 00:38:26 hobbs Exp $
 */

#include "tclInt.h"

/*
 * For each linked variable there is a data structure of the following
 * type, which describes the link and is the clientData for the trace
 * set on the Tcl variable.
 */

typedef struct Link {
    Tcl_Interp *interp;		/* Interpreter containing Tcl variable. */
    Tcl_Obj *varName;		/* Name of variable (must be global).  This
				 * is needed during trace callbacks, since
				 * the actual variable may be aliased at
				 * that time via upvar. */
    char *addr;			/* Location of C variable. */
    int type;			/* Type of link (TCL_LINK_INT, etc.). */
    union {
	int i;
	double d;
	Tcl_WideInt w;
    } lastValue;		/* Last known value of C variable;  used to
				 * avoid string conversions. */
    int flags;			/* Miscellaneous one-bit values;  see below
				 * for definitions. */
} Link;

/*
 * Definitions for flag bits:
 * LINK_READ_ONLY -		1 means errors should be generated if Tcl
 *				script attempts to write variable.
 * LINK_BEING_UPDATED -		1 means that a call to Tcl_UpdateLinkedVar
 *				is in progress for this variable, so
 *				trace callbacks on the variable should
 *				be ignored.
 */

#define LINK_READ_ONLY		1
#define LINK_BEING_UPDATED	2

/*
 * Forward references to procedures defined later in this file:
 */

static char *		LinkTraceProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp, char *name1, char *name2,
			    int flags));
static Tcl_Obj *	ObjValue _ANSI_ARGS_((Link *linkPtr));

/*
 *----------------------------------------------------------------------
 *
 * Tcl_LinkVar --
 *
 *	Link a C variable to a Tcl variable so that changes to either
 *	one causes the other to change.
 *
 * Results:
 *	The return value is TCL_OK if everything went well or TCL_ERROR
 *	if an error occurred (the interp's result is also set after
 *	errors).
 *
 * Side effects:
 *	The value at *addr is linked to the Tcl variable "varName",
 *	using "type" to convert between string values for Tcl and
 *	binary values for *addr.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_LinkVar(interp, varName, addr, type)
    Tcl_Interp *interp;		/* Interpreter in which varName exists. */
    char *varName;		/* Name of a global variable in interp. */
    char *addr;			/* Address of a C variable to be linked
				 * to varName. */
    int type;			/* Type of C variable: TCL_LINK_INT, etc. 
				 * Also may have TCL_LINK_READ_ONLY
				 * OR'ed in. */
{
    Tcl_Obj *objPtr;
    Link *linkPtr;
    int code;

    linkPtr = (Link *) ckalloc(sizeof(Link));
    linkPtr->interp = interp;
    linkPtr->varName = Tcl_NewStringObj(varName, -1);
    Tcl_IncrRefCount(linkPtr->varName);
    linkPtr->addr = addr;
    linkPtr->type = type & ~TCL_LINK_READ_ONLY;
    if (type & TCL_LINK_READ_ONLY) {
	linkPtr->flags = LINK_READ_ONLY;
    } else {
	linkPtr->flags = 0;
    }
    objPtr = ObjValue(linkPtr);
    if (Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, objPtr,
	    TCL_GLOBAL_ONLY|TCL_LEAVE_ERR_MSG) == NULL) {
	Tcl_DecrRefCount(linkPtr->varName);
	Tcl_DecrRefCount(objPtr);
	ckfree((char *) linkPtr);
	return TCL_ERROR;
    }
    code = Tcl_TraceVar(interp, varName, TCL_GLOBAL_ONLY|TCL_TRACE_READS
	    |TCL_TRACE_WRITES|TCL_TRACE_UNSETS, LinkTraceProc,
	    (ClientData) linkPtr);
    if (code != TCL_OK) {
	Tcl_DecrRefCount(linkPtr->varName);
	ckfree((char *) linkPtr);
    }
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UnlinkVar --
 *
 *	Destroy the link between a Tcl variable and a C variable.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If "varName" was previously linked to a C variable, the link
 *	is broken to make the variable independent.  If there was no
 *	previous link for "varName" then nothing happens.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_UnlinkVar(interp, varName)
    Tcl_Interp *interp;		/* Interpreter containing variable to unlink. */
    char *varName;		/* Global variable in interp to unlink. */
{
    Link *linkPtr;

    linkPtr = (Link *) Tcl_VarTraceInfo(interp, varName, TCL_GLOBAL_ONLY,
	    LinkTraceProc, (ClientData) NULL);
    if (linkPtr == NULL) {
	return;
    }
    Tcl_UntraceVar(interp, varName,
	    TCL_GLOBAL_ONLY|TCL_TRACE_READS|TCL_TRACE_WRITES|TCL_TRACE_UNSETS,
	    LinkTraceProc, (ClientData) linkPtr);
    Tcl_DecrRefCount(linkPtr->varName);
    ckfree((char *) linkPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_UpdateLinkedVar --
 *
 *	This procedure is invoked after a linked variable has been
 *	changed by C code.  It updates the Tcl variable so that
 *	traces on the variable will trigger.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The Tcl variable "varName" is updated from its C value,
 *	causing traces on the variable to trigger.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_UpdateLinkedVar(interp, varName)
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    char *varName;		/* Name of global variable that is linked. */
{
    Link *linkPtr;
    int savedFlag;

    linkPtr = (Link *) Tcl_VarTraceInfo(interp, varName, TCL_GLOBAL_ONLY,
	    LinkTraceProc, (ClientData) NULL);
    if (linkPtr == NULL) {
	return;
    }
    savedFlag = linkPtr->flags & LINK_BEING_UPDATED;
    linkPtr->flags |= LINK_BEING_UPDATED;
    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
	    TCL_GLOBAL_ONLY);
    linkPtr->flags = (linkPtr->flags & ~LINK_BEING_UPDATED) | savedFlag;
}

/*
 *----------------------------------------------------------------------
 *
 * LinkTraceProc --
 *
 *	This procedure is invoked when a linked Tcl variable is read,
 *	written, or unset from Tcl.  It's responsible for keeping the
 *	C variable in sync with the Tcl variable.
 *
 * Results:
 *	If all goes well, NULL is returned; otherwise an error message
 *	is returned.
 *
 * Side effects:
 *	The C variable may be updated to make it consistent with the
 *	Tcl variable, or the Tcl variable may be overwritten to reject
 *	a modification.
 *
 *----------------------------------------------------------------------
 */

static char *
LinkTraceProc(clientData, interp, name1, name2, flags)
    ClientData clientData;	/* Contains information about the link. */
    Tcl_Interp *interp;		/* Interpreter containing Tcl variable. */
    char *name1;		/* First part of variable name. */
    char *name2;		/* Second part of variable name. */
    int flags;			/* Miscellaneous additional information. */
{
    Link *linkPtr = (Link *) clientData;
    int changed, valueLength;
    CONST char *value;
    char **pp, *result;
    Tcl_Obj *objPtr, *valueObj;

    /*
     * If the variable is being unset, then just re-create it (with a
     * trace) unless the whole interpreter is going away.
     */

    if (flags & TCL_TRACE_UNSETS) {
	if (flags & TCL_INTERP_DESTROYED) {
	    Tcl_DecrRefCount(linkPtr->varName);
	    ckfree((char *) linkPtr);
	} else if (flags & TCL_TRACE_DESTROYED) {
	    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		    TCL_GLOBAL_ONLY);
	    Tcl_TraceVar(interp, Tcl_GetString(linkPtr->varName),
		    TCL_GLOBAL_ONLY|TCL_TRACE_READS|TCL_TRACE_WRITES
		    |TCL_TRACE_UNSETS, LinkTraceProc, (ClientData) linkPtr);
	}
	return NULL;
    }

    /*
     * If we were invoked because of a call to Tcl_UpdateLinkedVar, then
     * don't do anything at all.  In particular, we don't want to get
     * upset that the variable is being modified, even if it is
     * supposed to be read-only.
     */

    if (linkPtr->flags & LINK_BEING_UPDATED) {
	return NULL;
    }

    /*
     * For read accesses, update the Tcl variable if the C variable
     * has changed since the last time we updated the Tcl variable.
     */

    if (flags & TCL_TRACE_READS) {
	switch (linkPtr->type) {
	case TCL_LINK_INT:
	case TCL_LINK_BOOLEAN:
	    changed = *(int *)(linkPtr->addr) != linkPtr->lastValue.i;
	    break;
	case TCL_LINK_DOUBLE:
	    changed = *(double *)(linkPtr->addr) != linkPtr->lastValue.d;
	    break;
	case TCL_LINK_WIDE_INT:
	    changed = *(Tcl_WideInt *)(linkPtr->addr) != linkPtr->lastValue.w;
	    break;
	case TCL_LINK_STRING:
	    changed = 1;
	    break;
	default:
	    return "internal error: bad linked variable type";
	}
	if (changed) {
	    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		    TCL_GLOBAL_ONLY);
	}
	return NULL;
    }

    /*
     * For writes, first make sure that the variable is writable.  Then
     * convert the Tcl value to C if possible.  If the variable isn't
     * writable or can't be converted, then restore the varaible's old
     * value and return an error.  Another tricky thing: we have to save
     * and restore the interpreter's result, since the variable access
     * could occur when the result has been partially set.
     */

    if (linkPtr->flags & LINK_READ_ONLY) {
	Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		TCL_GLOBAL_ONLY);
	return "linked variable is read-only";
    }
    valueObj = Tcl_ObjGetVar2(interp, linkPtr->varName,NULL, TCL_GLOBAL_ONLY);
    if (valueObj == NULL) {
	/*
	 * This shouldn't ever happen.
	 */
	return "internal error: linked variable couldn't be read";
    }

    objPtr = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(objPtr);
    Tcl_ResetResult(interp);
    result = NULL;

    switch (linkPtr->type) {
    case TCL_LINK_INT:
	if (Tcl_GetIntFromObj(interp, valueObj, &linkPtr->lastValue.i)
		!= TCL_OK) {
	    Tcl_SetObjResult(interp, objPtr);
	    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		    TCL_GLOBAL_ONLY);
	    result = "variable must have integer value";
	    goto end;
	}
	*(int *)(linkPtr->addr) = linkPtr->lastValue.i;
	break;

    case TCL_LINK_WIDE_INT:
	if (Tcl_GetWideIntFromObj(interp, valueObj, &linkPtr->lastValue.w)
		!= TCL_OK) {
	    Tcl_SetObjResult(interp, objPtr);
	    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		    TCL_GLOBAL_ONLY);
	    result = "variable must have integer value";
	    goto end;
	}
	*(Tcl_WideInt *)(linkPtr->addr) = linkPtr->lastValue.w;
	break;

    case TCL_LINK_DOUBLE:
	if (Tcl_GetDoubleFromObj(interp, valueObj, &linkPtr->lastValue.d)
		!= TCL_OK) {
	    Tcl_SetObjResult(interp, objPtr);
	    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		    TCL_GLOBAL_ONLY);
	    result = "variable must have real value";
	    goto end;
	}
	*(double *)(linkPtr->addr) = linkPtr->lastValue.d;
	break;

    case TCL_LINK_BOOLEAN:
	if (Tcl_GetBooleanFromObj(interp, valueObj, &linkPtr->lastValue.i)
	    != TCL_OK) {
	    Tcl_SetObjResult(interp, objPtr);
	    Tcl_ObjSetVar2(interp, linkPtr->varName, NULL, ObjValue(linkPtr),
		    TCL_GLOBAL_ONLY);
	    result = "variable must have boolean value";
	    goto end;
	}
	*(int *)(linkPtr->addr) = linkPtr->lastValue.i;
	break;

    case TCL_LINK_STRING:
	value = Tcl_GetStringFromObj(valueObj, &valueLength);
	valueLength++;
	pp = (char **)(linkPtr->addr);
	if (*pp != NULL) {
	    ckfree(*pp);
	}
	*pp = (char *) ckalloc((unsigned) valueLength);
	memcpy(*pp, value, (unsigned) valueLength);
	break;

    default:
	return "internal error: bad linked variable type";
    }
    end:
    Tcl_DecrRefCount(objPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ObjValue --
 *
 *	Converts the value of a C variable to a Tcl_Obj* for use in a
 *	Tcl variable to which it is linked.
 *
 * Results:
 *	The return value is a pointer to a Tcl_Obj that represents
 *	the value of the C variable given by linkPtr.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
ObjValue(linkPtr)
    Link *linkPtr;		/* Structure describing linked variable. */
{
    char *p;

    switch (linkPtr->type) {
    case TCL_LINK_INT:
	linkPtr->lastValue.i = *(int *)(linkPtr->addr);
	return Tcl_NewIntObj(linkPtr->lastValue.i);
    case TCL_LINK_WIDE_INT:
	linkPtr->lastValue.w = *(Tcl_WideInt *)(linkPtr->addr);
	return Tcl_NewWideIntObj(linkPtr->lastValue.w);
    case TCL_LINK_DOUBLE:
	linkPtr->lastValue.d = *(double *)(linkPtr->addr);
	return Tcl_NewDoubleObj(linkPtr->lastValue.d);
    case TCL_LINK_BOOLEAN:
	linkPtr->lastValue.i = *(int *)(linkPtr->addr);
	return Tcl_NewBooleanObj(linkPtr->lastValue.i != 0);
    case TCL_LINK_STRING:
	p = *(char **)(linkPtr->addr);
	if (p == NULL) {
	    return Tcl_NewStringObj("NULL", 4);
	}
	return Tcl_NewStringObj(p, -1);

    /*
     * This code only gets executed if the link type is unknown
     * (shouldn't ever happen).
     */
    default:
	return Tcl_NewStringObj("??", 2);
    }
}
