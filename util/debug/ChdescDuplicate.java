import java.io.DataInput;
import java.io.IOException;

public class ChdescDuplicate extends Opcode
{
	public ChdescDuplicate(int original, int count, int blocks)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_DUPLICATE, "KDB_CHDESC_DUPLICATE", ChdescDuplicate.class);
		factory.addParameter("original", 4);
		factory.addParameter("count", 4);
		factory.addParameter("blocks", 4);
		return factory;
	}
}
