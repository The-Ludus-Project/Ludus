(function_definition) @function.around
(function_definition body: (compound_statement) @function.inside)
(parameter_declaration) @parameter.around
(struct_specifier) @class.around
(struct_specifier body: (field_declaration_list) @class.inside)
