#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# vim: set fileencoding=utf-8 :

import lxml

from libipk import errors

from lxml import etree

class Params :
	def __init__( self , filename = None , mode = None , namespaces = None ) :
		self.filename = filename
		self.changed = False
		self.doc = None
		self.nsmap = {}
		if filename :
			self.open( filename , mode , namespaces )

	def open( self , filename , mode = None , namespaces = None ) :
		if self.doc :
			raise IOError('File already opened')
		self.doc = etree.parse( filename )
		self.root = self.doc.getroot()
		self.changed = False

		# add default params.xml namespace
		if namespaces == None :
			self.add_namespace( 'ipk' , 'http://www.praterm.com.pl/SZARP/ipk' )

	def touch( self ) :
		self.changed = True

	def save( self , fn = None ) :
		if fn == None : fn = self.filename
		self.doc.write( fn , pretty_print=True )
		self.changed = False

	def close( self ) :
		return not self.changed 

	def add_namespace( self , name , url ) :
		self.nsmap[name] = url

	def set_namespaces( self , namespaces ) :
		self.nsmap = namespaces

	def apply( self , xpath , func , args = {} ) :
		for node in self.doc.xpath( xpath , namespaces = self.nsmap ) :
			func( node , **args )
