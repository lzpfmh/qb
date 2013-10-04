/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2012 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Chung Leong <cleong@cal.berkeley.edu>                        |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

static void qb_copy_address_dimensions(qb_compiler_context *cxt, qb_address *address, int32_t offset, qb_variable_dimensions *dim) {
	uint32_t i;
	dim->dimension_count = address->dimension_count + offset;
	if(dim->dimension_count > 0) {
		for(i = 0; i < dim->dimension_count; i++) {
			dim->dimension_addresses[i] = address->dimension_addresses[i];
		}
		if(offset == 0) {
			// the array sizes are the same
			for(i = 0; i < dim->dimension_count; i++) {
				dim->array_size_addresses[i] = address->array_size_addresses[i];
			}
		} else {
			// need to be recalculated
			for(i = dim->dimension_count - 1; (int32_t) i >= 0; i--) {
				if(i == dim->dimension_count - 1) {
					dim->array_size_addresses[i] = dim->dimension_addresses[i];
				} else {
					dim->array_size_addresses[i] = qb_obtain_on_demand_product(cxt, dim->dimension_addresses[i], dim->dimension_addresses[i + 1]);
				}
			}
		}
	} else {
		dim->array_size_addresses[0] = dim->dimension_addresses[0] = cxt->one_address;
	}
	dim->array_size_address = dim->array_size_addresses[0];
	dim->source_address = address;
}

static qb_address * qb_obtain_larger_of_two(qb_compiler_context *cxt, qb_address *size_address1, qb_address *value_address1, qb_address *size_address2, qb_address *value_address2) {
	if(size_address1 == value_address1 && size_address2 == value_address2) {
		qb_operand operands[2] = { { QB_OPERAND_ADDRESS, size_address1 }, { QB_OPERAND_ADDRESS, size_address2 } };
		return qb_obtain_on_demand_value(cxt, &factory_choose_size_of_larger_array_top_level, operands, 2);
	} else {
		qb_operand operands[4] = { { QB_OPERAND_ADDRESS, size_address1 }, { QB_OPERAND_ADDRESS, value_address1 }, { QB_OPERAND_ADDRESS, size_address2 }, { QB_OPERAND_ADDRESS, value_address2 } };
		return qb_obtain_on_demand_value(cxt, &factory_choose_size_of_larger_array, operands, 4);
	}
}

static void qb_choose_dimensions_from_two(qb_compiler_context *cxt, qb_variable_dimensions *dim1, qb_variable_dimensions *dim2, qb_variable_dimensions *dim) {
	if(FIXED_LENGTH(dim1) && FIXED_LENGTH(dim2)) {
		// we can figure one which to use now
		qb_variable_dimensions *dim_chosen;
		uint32_t i;
		if(SCALAR(dim1)) {
			// use the second if the first is a scalar
			dim_chosen = dim2;
		} else if(SCALAR(dim2)) {
			// use the first if the second is a scalar
			dim_chosen = dim1;
		} else if(ARRAY_SIZE(dim1) > ARRAY_SIZE(dim2)) {
			// use the first if it's bigger
			dim_chosen = dim1;
		} else if(ARRAY_SIZE(dim1) < ARRAY_SIZE(dim2)) {
			// use the second if it's bigger
			dim_chosen = dim2;
		} else if(dim2->dimension_count > dim1->dimension_count) {
			// use the second if it has more dimensions
			dim_chosen = dim2;
		} else {
			// use the first otherwise
			dim_chosen = dim1;
		}
		dim->dimension_count = dim_chosen->dimension_count;
		dim->source_address = dim_chosen->source_address;
		for(i = 0; i < dim->dimension_count; i++) {
			dim->dimension_addresses[i] = dim_chosen->dimension_addresses[i];
			dim->array_size_addresses[i] = dim_chosen->array_size_addresses[i];
		}
	} else {
		// have to choose at runtime
		uint32_t i;
		if(dim1->dimension_count == dim2->dimension_count) {
			// keep all the dimensions
			dim->dimension_count = dim1->dimension_count;
		} else {
			// impossible to resolve--just leave it flat
			dim->dimension_count = 1;
		}
		for(i = 0; i < dim->dimension_count; i++) {
			dim->dimension_addresses[i] = qb_obtain_larger_of_two(cxt, dim1->array_size_address, dim1->dimension_addresses[i], dim2->array_size_address, dim2->dimension_addresses[i]);
			dim->array_size_addresses[i] = qb_obtain_larger_of_two(cxt, dim1->array_size_address, dim1->array_size_addresses[i], dim2->array_size_address, dim2->array_size_addresses[i]);
		}
	}
	dim->array_size_address = dim->array_size_addresses[0];
}

static void qb_choose_dimensions_from_two_addresses(qb_compiler_context *cxt, qb_address *address1, int32_t offset1, qb_address *address2, int32_t offset2, qb_variable_dimensions *dim) {
	if(SCALAR(address1)) {
		qb_copy_address_dimensions(cxt, address2, offset2, dim);
	} else if(SCALAR(address2)) {
		qb_copy_address_dimensions(cxt, address1, offset1, dim);
	} else {
		qb_variable_dimensions _dim1, _dim2, *dim1 = &_dim1, *dim2 = &_dim2;
		qb_copy_address_dimensions(cxt, address1, offset1, dim1);
		qb_copy_address_dimensions(cxt, address2, offset2, dim2);

	}
}

static qb_address * qb_obtain_largest_of_three(qb_compiler_context *cxt, qb_address *size_address1, qb_address *value_address1, qb_address *size_address2, qb_address *value_address2, qb_address *size_address3, qb_address *value_address3) {
	if(size_address1 == value_address1 && size_address2 == value_address2 && size_address3 == value_address3) {
		qb_operand operands[3] = { { QB_OPERAND_ADDRESS, size_address1 }, { QB_OPERAND_ADDRESS, size_address2 }, { QB_OPERAND_ADDRESS, size_address3 } };
		return qb_obtain_on_demand_value(cxt, &factory_choose_size_of_largest_of_three_arrays_top_level, operands, 3);
	} else {
		qb_operand operands[6] = { { QB_OPERAND_ADDRESS, size_address1 }, { QB_OPERAND_ADDRESS, value_address1 }, { QB_OPERAND_ADDRESS, size_address2 }, { QB_OPERAND_ADDRESS, value_address2 }, { QB_OPERAND_ADDRESS, size_address3 }, { QB_OPERAND_ADDRESS, value_address3 } };
		return qb_obtain_on_demand_value(cxt, &factory_choose_size_of_largest_of_three_arrays, operands, 6);
	}
}

static void qb_choose_dimensions_from_three(qb_compiler_context *cxt, qb_variable_dimensions *dim1, qb_variable_dimensions *dim2, qb_variable_dimensions *dim3, qb_variable_dimensions *dim) {
	if(FIXED_LENGTH(dim1) && FIXED_LENGTH(dim2) && FIXED_LENGTH(dim3)) {
		// we can figure one which to use now
		qb_variable_dimensions *dim_chosen;
		uint32_t i;
		if(SCALAR(dim1) && SCALAR(dim2)) {
			// use the third if the first and second are scalars
			dim_chosen = dim3;
		} else if(SCALAR(dim1) && SCALAR(dim3)) {
			// use the second if the first and third are scalars
			dim_chosen = dim2;
		} else if(SCALAR(dim2) && SCALAR(dim3)) {
			// use the first if the second and third are scalars
			dim_chosen = dim1;
		} else if(ARRAY_SIZE(dim1) > ARRAY_SIZE(dim2) && ARRAY_SIZE(dim1) > ARRAY_SIZE(dim3)) {
			// use the first if it's bigger
			dim_chosen = dim1;
		} else if(ARRAY_SIZE(dim2) > ARRAY_SIZE(dim1) && ARRAY_SIZE(dim2) > ARRAY_SIZE(dim3)) {
			// use the second if it's bigger
			dim_chosen = dim2;
		} else if(ARRAY_SIZE(dim3) > ARRAY_SIZE(dim1) && ARRAY_SIZE(dim3) > ARRAY_SIZE(dim2)) {
			// use the third if it's bigger
			dim_chosen = dim3;
		} else if(dim3->dimension_count > dim2->dimension_count && dim3->dimension_count > dim2->dimension_count) {
			// use the third if it has more dimensions
			dim_chosen = dim3;
		} else if(dim2->dimension_count > dim1->dimension_count && dim2->dimension_count > dim3->dimension_count) {
			// use the second if it has more dimensions
			dim_chosen = dim2;
		} else {
			// use the first otherwise
			dim_chosen = dim1;
		}
		dim->dimension_count = dim_chosen->dimension_count;
		dim->source_address = dim_chosen->source_address;
		for(i = 0; i < dim->dimension_count; i++) {
			dim->dimension_addresses[i] = dim_chosen->dimension_addresses[i];
			dim->array_size_addresses[i] = dim_chosen->array_size_addresses[i];
		}
	} else if(SCALAR(dim1)) {
		// only need to choose between the second and third
		qb_choose_dimensions_from_two(cxt, dim2, dim3, dim);
		return;
	} else if(SCALAR(dim2)) {
		// only need to choose between the first and third
		qb_choose_dimensions_from_two(cxt, dim1, dim3, dim);
		return;
	} else if(SCALAR(dim3)) {
		// only need to choose between the first and second
		qb_choose_dimensions_from_two(cxt, dim1, dim2, dim);
		return;
	} else {
		// have to choose at runtime
		uint32_t i;
		if(dim1->dimension_count == dim2->dimension_count && dim1->dimension_count == dim3->dimension_count) {
			// keep all the dimensions
			dim->dimension_count = dim1->dimension_count;
		} else {
			// impossible to resolve--just leave it flat
			dim->dimension_count = 1;
		}
		for(i = 0; i < dim->dimension_count; i++) {
			dim->dimension_addresses[i] = qb_obtain_largest_of_three(cxt, dim1->array_size_address, dim1->dimension_addresses[i], dim2->array_size_address, dim2->dimension_addresses[i], dim3->array_size_address, dim3->dimension_addresses[i]);
			dim->array_size_addresses[i] = qb_obtain_largest_of_three(cxt, dim1->array_size_address, dim1->array_size_addresses[i], dim2->array_size_address, dim2->array_size_addresses[i], dim3->array_size_address, dim3->array_size_addresses[i]);
		}
	}
	dim->array_size_address = dim->array_size_addresses[0];
}

static void qb_choose_dimensions_from_three_addresses(qb_compiler_context *cxt, qb_address *address1, int32_t offset1, qb_address *address2, int32_t offset2, qb_address *address3, int32_t offset3, qb_variable_dimensions *dim) {
	if(SCALAR(address1) && SCALAR(address2)) {
		qb_copy_address_dimensions(cxt, address3, offset3, dim);
	} else if(SCALAR(address1) && SCALAR(address3)) {
		qb_copy_address_dimensions(cxt, address2, offset2, dim);
	} else if(SCALAR(address2) && SCALAR(address3)) {
		qb_copy_address_dimensions(cxt, address1, offset1, dim);
	} else {
		qb_variable_dimensions dim1, dim2, dim3;
		qb_copy_address_dimensions(cxt, address1, offset1, &dim1);
		qb_copy_address_dimensions(cxt, address2, offset2, &dim2);
		qb_copy_address_dimensions(cxt, address3, offset3, &dim3);
		qb_choose_dimensions_from_three(cxt, &dim1, &dim2, &dim3, dim);
	}
}

static void qb_set_result_dimensions_first_operand(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_operand *first = &operands[0];
	qb_copy_address_dimensions(cxt, first->address, 0, dim);
}

static void qb_set_result_dimensions_second_operand(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_operand *second = &operands[1];
	qb_copy_address_dimensions(cxt, second->address, 0, dim);
}

static void qb_set_result_dimensions_larger_of_two(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_operand *first = &operands[0], *second = &operands[1];
	qb_choose_dimensions_from_two_addresses(cxt, first->address, 0, second->address, 0, dim);
}

static void qb_set_result_dimensions_largest_of_three(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_operand *first = &operands[0], *second = &operands[1], *third = &operands[2];
	qb_choose_dimensions_from_three_addresses(cxt, first->address, 0, second->address, 0, third->address, 0, dim);
}

static void qb_set_result_dimensions_vector_count(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_address *vector_address = operands[0].address;
	qb_copy_address_dimensions(cxt, vector_address, -1, dim);
}

static qb_matrix_order qb_get_matrix_order(qb_compiler_context *cxt, qb_op_factory *f) {
	if(f->result_flags & QB_RESULT_IS_COLUMN_MAJOR) {
		return QB_MATRIX_ORDER_COLUMN_MAJOR;
	} else if(f->result_flags & QB_RESULT_IS_ROW_MAJOR) {
		return QB_MATRIX_ORDER_ROW_MAJOR;
	} else {
		return cxt->matrix_order;
	}
}

static void qb_set_result_dimensions_mm_mult(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	/*
	qb_address *m1_address, *m1_size_address, *m1_row_address;
	qb_address *m2_address, *m2_size_address, *m2_col_address;
	qb_address *m1_count_address, *m2_count_address;
	uint32_t i, m1_count, m2_count, res_count;

	qb_matrix_order order = qb_get_matrix_order(cxt, f);
	int32_t col_offset = (order == QB_MATRIX_ORDER_COLUMN_MAJOR) ? -2 : -1;
	int32_t row_offset = (order == QB_MATRIX_ORDER_ROW_MAJOR) ? -2 : -1;

	m1_address = operands[0].address;
	m2_address = operands[1].address;
	m1_size_address = m1_address->array_size_addresses[m1_address->dimension_count - 2];
	m2_size_address = m2_address->array_size_addresses[m2_address->dimension_count - 2];
	m1_row_address = m1_address->dimension_addresses[m1_address->dimension_count + row_offset];
	m2_col_address = m2_address->dimension_addresses[m2_address->dimension_count + col_offset];
	*/
}

static void qb_set_result_dimensions_mv_mult(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	/*
	qb_address *m_address, *m_size_address;
	qb_address *v_address, *v_width_address;
	uint32_t i, m_count, v_count, res_count;

	m_address = operands[0].address;
	v_address = operands[1].address;
	v_width_address = v_address->array_size_addresses[v_address->dimension_count - 1];
	m_size_address = m_address->array_size_addresses[m_address->dimension_count - 2];
	*/
}

static void qb_set_result_dimensions_vm_mult(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	// TODO: I think this is broken when the matrix isn't square
	qb_operand reversed_operands[2];
	reversed_operands[0] = operands[1];
	reversed_operands[1] = operands[0];
	qb_set_result_dimensions_mv_mult(cxt, f, reversed_operands, operand_count, dim);
}

// make use of the fact that A' * B' = (B * A)' 
static void qb_set_result_dimensions_matrix_equivalent(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_derived_op_factory *df = (qb_derived_op_factory *) f;
	qb_operand reversed_operands[2];
	reversed_operands[0] = operands[1];
	reversed_operands[1] = operands[0];
	f = df->parent;
	f->set_dimensions(cxt, f, reversed_operands, operand_count, dim);
}

static void qb_set_result_dimensions_matrix_current_mode(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_matrix_op_factory_selector *s = (qb_matrix_op_factory_selector *) f;
	if(cxt->matrix_order == QB_MATRIX_ORDER_COLUMN_MAJOR) {
		f = s->cm_factory;
	} else {
		f = s->rm_factory;
	}
	f->set_dimensions(cxt, f, operands, operand_count, dim);
}

static void qb_set_result_dimensions_transpose(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_address *matrix_address = operands[0].address, *temp;
	qb_copy_address_dimensions(cxt, matrix_address, 0, dim);

	// swap the lowest two dimensions
	temp = dim->dimension_addresses[dim->dimension_count - 2];
	dim->dimension_addresses[dim->dimension_count - 2] = dim->dimension_addresses[dim->dimension_count - 1];
	dim->dimension_addresses[dim->dimension_count - 1] = dim->array_size_addresses[dim->dimension_count - 1] = temp;
	dim->source_address = matrix_address;
}

static void qb_set_result_dimensions_determinant(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_address *matrix_address = operands[0].address;
	qb_copy_address_dimensions(cxt, matrix_address, -2, dim);
}

static void qb_set_result_dimensions_sampling(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	/*
	qb_address *image_address = operands[0].address;
	qb_address *x_address = operands[1].address;
	qb_address *y_address = operands[2].address;
	qb_address *channel_count_address = image_address->dimension_addresses[image_address->dimension_count - 1];
	uint32_t channel_count = VALUE(U32, channel_count_address), i;
	*/
}

static void qb_set_result_dimensions_array_merge(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	/*
	uint32_t i, final_length = 0;
	qb_address *dimension_source = NULL;
	for(i = 0; i < operand_count; i++) {
		qb_address *address = operands[i].address;
		if(VARIABLE_LENGTH_ARRAY(address)) {
			final_length = 0;
			dimension_source = NULL;
			break;
		} else {
			if(SCALAR(address)) {
				final_length += 1;
			} else {
				final_length += ARRAY_SIZE(address);
			}
		}
		if(!dimension_source || address->dimension_count > dimension_source->dimension_count) {
			dimension_source = address;
		}
	}
	dim->array_size = final_length;
	if(dimension_source && dimension_source->dimension_count > 1) {
		uint32_t element_count = final_length / VALUE(U32, dimension_source->array_size_addresses[1]);
		dim->dimension_addresses[0] = qb_obtain_constant_U32(cxt, element_count);
		for(i = 1; i < dimension_source->dimension_count; i++) {
			dim->dimension_addresses[i] = dimension_source->dimension_addresses[i];
		}
		dim->dimension_count = dimension_source->dimension_count;
	} else {
		dim->dimension_count = 1;
	}
	*/
}

static void qb_set_result_dimensions_array_fill(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	/*
	qb_address *index_address = operands[0].address;
	qb_address *number_address = operands[1].address;
	qb_address *value_address = operands[2].address;
	int32_t count_is_constant;
	uint32_t count;
	if(CONSTANT(index_address) && CONSTANT(number_address)) {
		uint32_t start_index = VALUE(U32, index_address);
		uint32_t number = VALUE(U32, number_address);
		count = start_index + number;
		count_is_constant = TRUE;
	} else {
		count_is_constant = FALSE;
	}

	if(count_is_constant && !VARIABLE_LENGTH_ARRAY(value_address)) {
		uint32_t value_size;
		if(SCALAR(value_address)) {
			value_size = 1;
		} else {
			value_size = ARRAY_SIZE(value_address);
		}
		dim->array_size = count * value_size;
	} else {
		dim->array_size = 0;
	}
	if(value_address->dimension_count > 0) {
		uint32_t i;
		dim->dimension_count = value_address->dimension_count + 1;
		for(i = 0; i < value_address->dimension_count; i++) {
			dim->dimension_addresses[i + 1] = value_address->dimension_addresses[i];
		}
		if(count_is_constant) {
			dim->dimension_addresses[0] = qb_obtain_constant_U32(cxt, count);
		} else {
			dim->dimension_addresses[0] = NULL;
		}
	} else {
		dim->dimension_count = 1;
	}
	*/
}

static void qb_set_result_dimensions_array_pad(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_address *container_address = operands[0].address;
	/*
	dim->array_size = 0;
	if(container_address->dimension_count > 1) {
		uint32_t i;
		dim->dimension_count = container_address->dimension_count;
		for(i = 1; i < container_address->dimension_count; i++) {
			dim->dimension_addresses[i] = container_address->dimension_addresses[i];
		}
		dim->dimension_addresses[0] = NULL;
	} else {
		dim->dimension_count = 1;
	}
	*/
}

static void qb_set_result_dimensions_array_column(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_address *address1 = operands[0].address;
	/*
	if(VARIABLE_LENGTH_ARRAY(address1)) {
		dim->array_size = 0;
	} else {
		dim->array_size = VALUE(U32, address1->dimension_addresses[0]);
		if(address1->dimension_count > 2) {
			dim->array_size *= VALUE(U32, address1->array_size_addresses[2]);
		}
	}
	if(address1->dimension_count > 2) {
		uint32_t i;
		dim->dimension_count = address1->dimension_count - 1;
		dim->dimension_addresses[0] = address1->dimension_addresses[0];
		for(i = 2; i < address1->dimension_count; i++) {
			dim->dimension_addresses[i - 1] = address1->dimension_addresses[i];
		}
	} else {
		dim->dimension_count = 1;
	}
	*/
}

uint32_t qb_get_range_length_F32(float32_t op1, float32_t op2, float32_t op3);
uint32_t qb_get_range_length_F64(float64_t op1, float64_t op2, float64_t op3);
uint32_t qb_get_range_length_S08(int8_t op1, int8_t op2, int8_t op3);
uint32_t qb_get_range_length_S16(int16_t op1, int16_t op2, int16_t op3);
uint32_t qb_get_range_length_S32(int32_t op1, int32_t op2, int32_t op3);
uint32_t qb_get_range_length_S64(int64_t op1, int64_t op2, int64_t op3);
uint32_t qb_get_range_length_U08(uint8_t op1, uint8_t op2, int8_t op3);
uint32_t qb_get_range_length_U16(uint16_t op1, uint16_t op2, int16_t op3);
uint32_t qb_get_range_length_U32(uint32_t op1, uint32_t op2, int32_t op3);
uint32_t qb_get_range_length_U64(uint64_t op1, uint64_t op2, int64_t op3);

static void qb_set_result_dimensions_range(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	// the result size is known only if the all operands are constant
	qb_address *start_address = operands[0].address;
	qb_address *end_address = operands[1].address;
	qb_address *interval_address = (operand_count >= 3) ? operands[2].address : NULL;
	uint32_t current_array_size = UINT32_MAX;
	if(CONSTANT(start_address) && CONSTANT(end_address) && (!interval_address || CONSTANT(interval_address))) {
		switch(start_address->type) {
			case QB_TYPE_S08: current_array_size = qb_get_range_length_S08(VALUE(S08, start_address), VALUE(S08, end_address), (interval_address) ? VALUE(S08, interval_address) : 1); break;
			case QB_TYPE_U08: current_array_size = qb_get_range_length_U08(VALUE(U08, start_address), VALUE(U08, end_address), (interval_address) ? VALUE(U08, interval_address) : 1); break;
			case QB_TYPE_S16: current_array_size = qb_get_range_length_S16(VALUE(S16, start_address), VALUE(S16, end_address), (interval_address) ? VALUE(S16, interval_address) : 1); break;
			case QB_TYPE_U16: current_array_size = qb_get_range_length_U16(VALUE(U16, start_address), VALUE(U16, end_address), (interval_address) ? VALUE(U16, interval_address) : 1); break;
			case QB_TYPE_S32: current_array_size = qb_get_range_length_S32(VALUE(S32, start_address), VALUE(S32, end_address), (interval_address) ? VALUE(S32, interval_address) : 1); break;
			case QB_TYPE_U32: current_array_size = qb_get_range_length_U32(VALUE(U32, start_address), VALUE(U32, end_address), (interval_address) ? VALUE(U32, interval_address) : 1); break;
			case QB_TYPE_S64: current_array_size = qb_get_range_length_S64(VALUE(S64, start_address), VALUE(S64, end_address), (interval_address) ? VALUE(S64, interval_address) : 1); break;
			case QB_TYPE_U64: current_array_size = qb_get_range_length_U64(VALUE(U64, start_address), VALUE(U64, end_address), (interval_address) ? VALUE(U64, interval_address) : 1); break;
			case QB_TYPE_F32: current_array_size = qb_get_range_length_F32(VALUE(F32, start_address), VALUE(F32, end_address), (interval_address) ? VALUE(F32, interval_address) : 1); break;
			case QB_TYPE_F64: current_array_size = qb_get_range_length_F64(VALUE(F64, start_address), VALUE(F64, end_address), (interval_address) ? VALUE(F64, interval_address) : 1); break;
			default: break;
		}
	}
	/*
	dim->dimension_count = 1;
	if(current_array_size != UINT32_MAX) {
		dim->array_size = current_array_size;
	} else {
		// the array size isn't known (or the parameters are erroreous)
		dim->array_size = 0;
	}
	*/
}

static void qb_set_result_dimensions_array_rand(qb_compiler_context *cxt, qb_op_factory *f, qb_operand *operands, uint32_t operand_count, qb_variable_dimensions *dim) {
	qb_address *count_address = operands[1].address;
	/*
	if(CONSTANT(count_address)) {
		uint32_t count = VALUE(U32, count_address);
		dim->array_size = count;
		dim->dimension_count = (count == 1) ? 0 : 1;
	} else {
		// don't know how many elements will be returned
		dim->dimension_count = 1;
		dim->array_size = 0;
	}
	*/
}
