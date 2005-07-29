import java.io.DataInput;
import java.io.IOException;

public class ChdescConvertByte extends Opcode
{
	private final int chdesc;
	private final short offset, length;
	
	public ChdescConvertByte(int chdesc, short offset, short length)
	{
		this.chdesc = chdesc;
		this.offset = offset;
		this.length = length;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_CONVERT_BYTE, "KDB_CHDESC_CONVERT_BYTE", ChdescConvertByte.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("offset", 2);
		factory.addParameter("length", 2);
		return factory;
	}
}
