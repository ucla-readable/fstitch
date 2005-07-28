import java.io.DataInput;
import java.io.IOException;

public class ChdescConvertBit extends Opcode
{
	public ChdescConvertBit(int chdesc, short offset, int xor)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CONVERT_BIT, "KDB_CHDESC_CONVERT_BIT", ChdescConvertBit.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("xor", 4);
		return factory;
	}
}
