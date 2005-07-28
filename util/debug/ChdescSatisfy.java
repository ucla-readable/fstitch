import java.io.DataInput;
import java.io.IOException;

public class ChdescSatisfy extends Opcode
{
	public ChdescSatisfy(int chdesc)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SATISFY, "KDB_CHDESC_SATISFY", ChdescSatisfy.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
